"""Baseline lifecycle: capture from trusted windows, persist, activate, load.

Statistics are always computed by the C core (alga_baseline_builder_*), from
windows that the core itself marked trusted and classified into the target
operating mode.  A captured baseline starts as `candidate`; it influences
decisions only after an explicit, audited activation.
"""

from __future__ import annotations

import ctypes
import json
from dataclasses import dataclass, field
from typing import Any

from ..audit.canonical import canonical_json, finite_or_none
from ..core.libalgaops import (
    FEATURES,
    MAX_FEATURES,
    MODES,
    AlgaBaseline,
    AlgaBaselineBuilder,
    AlgaDecision,
    AlgaWindow,
    CoreError,
    CoreLibrary,
)


def mode_index(name: str) -> int:
    try:
        idx = MODES.index(name.upper())
    except ValueError:
        raise ValueError(f"unknown mode {name!r}") from None
    if idx == 0:
        raise ValueError("baselines exist only for OFF, STARTUP and STEADY")
    return idx


def baseline_to_stats(bl: AlgaBaseline) -> dict[str, Any]:
    feats = [FEATURES[i] for i in range(MAX_FEATURES) if bl.feature_mask & (1 << i)]
    idx = [i for i in range(MAX_FEATURES) if bl.feature_mask & (1 << i)]
    return {
        "mode": MODES[bl.mode],
        "n": int(bl.n),
        "feature_mask": int(bl.feature_mask),
        "features": feats,
        "mean": {FEATURES[i]: finite_or_none(bl.mean[i]) for i in idx},
        "std": {FEATURES[i]: finite_or_none(bl.std[i]) for i in idx},
        "cov": [[finite_or_none(bl.cov[i * MAX_FEATURES + j]) for j in idx] for i in idx],
    }


def stats_to_baseline(stats: dict[str, Any], baseline_id: int) -> AlgaBaseline:
    bl = AlgaBaseline()
    bl.id = baseline_id
    bl.mode = mode_index(stats["mode"])
    bl.feature_mask = int(stats["feature_mask"])
    bl.n = int(stats["n"])
    bl.valid = 1
    idx = [i for i in range(MAX_FEATURES) if bl.feature_mask & (1 << i)]
    for a, i in enumerate(idx):
        name = FEATURES[i]
        bl.mean[i] = float(stats["mean"][name])
        bl.std[i] = float(stats["std"][name])
        for b, j in enumerate(idx):
            bl.cov[i * MAX_FEATURES + j] = float(stats["cov"][a][b])
    return bl


@dataclass
class CaptureSession:
    node_id: str
    mode: int
    target: int
    started_ms: int
    builder: AlgaBaselineBuilder = field(default_factory=AlgaBaselineBuilder)
    accepted: int = 0
    skipped_untrusted: int = 0
    skipped_mode: int = 0
    first_seq: int | None = None
    last_seq: int | None = None

    def status(self) -> dict[str, Any]:
        return {"node_id": self.node_id, "mode": MODES[self.mode], "target": self.target,
                "accepted": self.accepted, "skipped_untrusted": self.skipped_untrusted,
                "skipped_other_mode": self.skipped_mode, "started_ms": self.started_ms}


class BaselineCapture:
    """Accumulates trusted, mode-matching windows through the core builder."""

    def __init__(self, lib: CoreLibrary, feature_mask: int) -> None:
        self.lib = lib
        self.feature_mask = feature_mask
        self.sessions: dict[str, CaptureSession] = {}

    def start(self, node_id: str, mode: str, target: int, now_ms: int) -> CaptureSession:
        if target < 2:
            raise ValueError("a baseline needs at least 2 windows")
        s = CaptureSession(node_id, mode_index(mode), int(target), now_ms)
        self.lib.raw.alga_baseline_builder_init(ctypes.byref(s.builder), self.feature_mask)
        self.sessions[node_id] = s
        return s

    def cancel(self, node_id: str) -> CaptureSession | None:
        return self.sessions.pop(node_id, None)

    def offer(self, node_id: str, window: AlgaWindow, decision: AlgaDecision
              ) -> CaptureSession | None:
        """Feed one processed window; returns the session when it is complete."""
        s = self.sessions.get(node_id)
        if s is None:
            return None
        if not decision.is_trusted:
            s.skipped_untrusted += 1
            return None
        if decision.mode != s.mode:
            s.skipped_mode += 1
            return None
        st = self.lib.raw.alga_baseline_builder_add_window(ctypes.byref(s.builder),
                                                           ctypes.byref(window))
        if st != 0:
            s.skipped_untrusted += 1
            return None
        s.accepted += 1
        s.first_seq = s.first_seq if s.first_seq is not None else int(window.sequence_id)
        s.last_seq = int(window.sequence_id)
        if s.accepted >= s.target:
            return self.sessions.pop(node_id)
        return None

    def finish(self, s: CaptureSession, min_samples: int) -> AlgaBaseline:
        out = AlgaBaseline()
        st = self.lib.raw.alga_baseline_builder_finish(ctypes.byref(s.builder), s.mode,
                                                       int(min_samples), ctypes.byref(out))
        if st != 0:
            raise CoreError("alga_baseline_builder_finish", st,
                            f"{s.accepted} samples (< {min_samples}) or no common features")
        return out


def stats_json(stats: dict[str, Any]) -> str:
    return canonical_json(stats)


def parse_stats(text: str) -> dict[str, Any]:
    return json.loads(text)
