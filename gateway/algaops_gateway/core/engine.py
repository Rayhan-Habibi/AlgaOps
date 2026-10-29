"""Per-node wrapper around the C diagnostic engine, and policy loading.

The policy file is the same flat TOML used by the C CLI.  It is applied key by
key through algaops_config_set_*; the *effective* configuration (every key,
read back from the core) is hashed with Ascon-Hash256 and that policy_hash is
recorded on every decision.
"""

from __future__ import annotations

import ctypes
import logging
import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from ..audit.canonical import canonical_bytes
from .libalgaops import (
    MODE_COUNT,
    OUTCOMES,
    AlgaBaseline,
    AlgaDecision,
    AlgaWindow,
    CoreError,
    CoreLibrary,
    DecisionCallback,
)

log = logging.getLogger(__name__)


def _flatten(prefix: str, value: Any, out: dict[str, Any]) -> None:
    if isinstance(value, dict):
        for k, v in value.items():
            _flatten(f"{prefix}.{k}" if prefix else k, v, out)
    else:
        out[prefix] = value


@dataclass(frozen=True)
class Policy:
    path: str | None
    meta: dict[str, Any]
    settings: dict[str, Any]
    effective: dict[str, Any] = field(default_factory=dict)
    hash: str = ""

    def apply(self, lib: CoreLibrary, cfg) -> None:
        for key, value in self.settings.items():
            lib.config_set(cfg, key, value)
        lib.config_validate(cfg)


def load_policy(lib: CoreLibrary, path: str | Path | None) -> Policy:
    """Load and validate a policy file (None => built-in legacy defaults)."""
    settings: dict[str, Any] = {}
    meta: dict[str, Any] = {}
    if path is not None:
        with open(path, "rb") as fh:
            data = tomllib.load(fh)
        meta = dict(data.pop("meta", {}))
        _flatten("", data, settings)
        unknown = sorted(k for k in settings if k not in lib.config_keys)
        if unknown:
            raise CoreError("load_policy", -7, f"unknown policy key(s): {', '.join(unknown)}")
        missing = sorted(set(lib.config_keys) - set(settings))
        if missing:
            log.warning("policy %s does not set %d key(s); core defaults apply: %s", path,
                        len(missing), ", ".join(missing))
    probe = Policy(str(path) if path else None, meta, settings)
    cfg = lib.new_config()
    probe.apply(lib, cfg)
    effective = lib.config_effective(cfg)
    digest = lib.ascon_hash256(canonical_bytes(effective)).hex()
    return Policy(probe.path, meta, settings, effective, digest)


class NodeEngine:
    """One C engine instance per sensor node (pipeline worker thread only)."""

    def __init__(self, lib: CoreLibrary, policy: Policy, node_id: str) -> None:
        self.lib = lib
        self.node_id = node_id
        self.policy = policy
        self._cfg = lib.new_config()
        policy.apply(lib, self._cfg)
        handle = lib.raw.algaops_engine_create(self._cfg)
        if not handle:
            raise CoreError("algaops_engine_create", -7, "engine rejected the configuration")
        self._h = ctypes.c_void_p(handle)
        self._dec = AlgaDecision()

    # ---- lifecycle -------------------------------------------------------------
    def close(self) -> None:
        if self._h:
            self.lib.raw.algaops_engine_destroy(self._h)
            self._h = ctypes.c_void_p()

    def __del__(self) -> None:  # pragma: no cover - best effort
        try:
            self.close()
        except Exception:
            pass

    def reset(self) -> None:
        self.lib.raw.algaops_engine_reset(self._h)

    # ---- decisions -------------------------------------------------------------
    def process(self, window: AlgaWindow) -> AlgaDecision:
        st = self.lib.raw.algaops_engine_process(self._h, ctypes.byref(window),
                                                 ctypes.byref(self._dec))
        if st != 0:
            raise CoreError("algaops_engine_process", st)
        return AlgaDecision.from_buffer_copy(self._dec)

    def tick(self, now_ms: int) -> AlgaDecision | None:
        emitted = ctypes.c_int(0)
        st = self.lib.raw.algaops_engine_tick(self._h, ctypes.c_uint64(now_ms),
                                              ctypes.byref(self._dec), ctypes.byref(emitted))
        if st != 0:
            raise CoreError("algaops_engine_tick", st)
        return AlgaDecision.from_buffer_copy(self._dec) if emitted.value else None

    def malformed(self, received_ms: int, detail: str) -> AlgaDecision:
        st = self.lib.raw.algaops_engine_report_malformed(
            self._h, ctypes.c_uint64(received_ms), detail.encode("utf-8", "replace")[:900],
            ctypes.byref(self._dec))
        if st != 0:
            raise CoreError("algaops_engine_report_malformed", st)
        return AlgaDecision.from_buffer_copy(self._dec)

    def restore_alert(self, kind: str | None) -> None:
        """Align the engine's alert state with the persisted open alert (warm start)."""
        code = -1 if kind is None else OUTCOMES.index(kind)
        st = self.lib.raw.algaops_engine_restore_alert(self._h, ctypes.c_int32(code))
        if st != 0:
            raise CoreError("algaops_engine_restore_alert", st)

    # ---- baselines ---------------------------------------------------------------
    def set_baseline(self, baseline: AlgaBaseline) -> None:
        st = self.lib.raw.algaops_engine_set_baseline(self._h, ctypes.byref(baseline))
        if st != 0:
            raise CoreError("algaops_engine_set_baseline", st, "baseline rejected by the core")

    def clear_baseline(self, mode: int) -> None:
        self.lib.raw.algaops_engine_clear_baseline(self._h, mode)

    def get_baseline(self, mode: int) -> AlgaBaseline | None:
        out = AlgaBaseline()
        st = self.lib.raw.algaops_engine_get_baseline(self._h, mode, ctypes.byref(out))
        return out if st == 0 else None


def replay_csv(lib: CoreLibrary, policy: Policy, csv_path: str | Path,
               baseline_csv: str | Path | None = None) -> list[tuple[int, AlgaDecision, bool]]:
    """Run a v1/v2 CSV through a fresh engine using the C CSV reader.

    Returns (row, decision, parse_error) tuples.  Used for parity tests
    against the C CLI and for baseline import.
    """
    engine = NodeEngine(lib, policy, "csv")
    try:
        if baseline_csv is not None:
            for bl in baselines_from_csv(lib, policy, baseline_csv):
                engine.set_baseline(bl)
        rows: list[tuple[int, AlgaDecision, bool]] = []

        @DecisionCallback
        def cb(user, row, w, d, parse_error):
            rows.append((int(row), AlgaDecision.from_buffer_copy(d.contents), bool(parse_error)))

        n = ctypes.c_int(0)
        st = lib.raw.algaops_replay_csv(engine._h, str(csv_path).encode(), cb, None,
                                        ctypes.byref(n))
        if st != 0:
            raise CoreError("algaops_replay_csv", st, str(csv_path))
        return rows
    finally:
        engine.close()


def baselines_from_csv(lib: CoreLibrary, policy: Policy, csv_path: str | Path
                       ) -> list[AlgaBaseline]:
    cfg = lib.new_config()
    policy.apply(lib, cfg)
    arr = (AlgaBaseline * MODE_COUNT)()
    n = ctypes.c_int(0)
    st = lib.raw.algaops_baselines_from_csv(cfg, str(csv_path).encode(), arr, ctypes.byref(n))
    if st == -5:          # ALGAOPS_ERR_BASELINE: no mode reached baseline.min_samples
        return []
    if st != 0:
        raise CoreError("algaops_baselines_from_csv", st, str(csv_path))
    return [AlgaBaseline.from_buffer_copy(arr[m]) for m in range(1, MODE_COUNT) if arr[m].valid]
