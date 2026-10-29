"""Strict validation of telemetry_window.v1 payloads (schemas/telemetry_window.v1.schema.json).

Structural problems (not JSON, wrong schema version, unknown or mistyped
fields, identity mismatch) raise PayloadError and become MALFORMED decisions.
Measurement values that are null / NaN / Infinity are NOT structural errors:
they are passed to the core as NaN so that TrustGuard reports NON_FINITE with
the window identity attached.  The gateway never repairs or interpolates data.
"""

from __future__ import annotations

import json
import math
import re
from dataclasses import dataclass, field
from typing import Any

from ..audit.canonical import finite_or_none
from ..core.libalgaops import (
    HAS_AXES,
    HAS_BANDS,
    HAS_CURRENT,
    HAS_NODE_FAULTS,
    HAS_P2P,
    HAS_RECEIVED,
    HAS_SAMPLE_COUNTS,
    HAS_SELF_TEST,
    HAS_TIME_SYNC,
    HAS_TIMING,
    MAX_BANDS,
    NODE_FAULTS,
    SELF_TEST,
    AlgaWindow,
)

SCHEMA_ID = "algaops.telemetry_window.v1"
MAX_PAYLOAD_BYTES = 8192
NODE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
SOURCES = ("hardware", "simulator", "replay")
U32_MAX = 0xFFFFFFFF
U63_MAX = (1 << 63) - 1

TOP_KEYS = {
    "schema", "node_id", "boot_id", "seq", "t_start_ms", "t_end_ms", "window_ms", "time_synced",
    "sample_rate_hz", "samples_expected", "samples_valid", "vib", "temp_c", "current_a",
    "self_test", "faults", "fw", "source", "health",
}
REQUIRED = ("schema", "node_id", "boot_id", "seq", "t_end_ms", "vib", "temp_c", "source")
VIB_KEYS = {"rms_g", "rms_axis_g", "p2p_g", "band_rms_g", "bands_hz"}
HEALTH_KEYS = {"i2c_errors", "ow_crc_errors", "fifo_overruns", "queue_depth", "dropped_windows",
               "rssi_dbm", "uptime_s", "reconnects"}


class PayloadError(ValueError):
    def __init__(self, reason: str, node_hint: str | None = None) -> None:
        super().__init__(reason)
        self.reason = reason
        self.node_hint = node_hint


@dataclass
class NormalizedWindow:
    node_id: str
    boot_id: int
    seq: int
    t_end_ms: int
    received_ms: int
    source: str
    t_start_ms: int | None = None
    window_ms: int | None = None
    time_synced: bool | None = None
    sample_rate_hz: float | None = None
    samples_expected: int | None = None
    samples_valid: int | None = None
    vib_rms_g: float = math.nan
    vib_rms_axis_g: list[float] | None = None
    vib_p2p_g: list[float] | None = None
    band_rms_g: list[float] | None = None
    bands_hz: list[list[float]] | None = None
    temp_c: float = math.nan
    current_present: bool = False
    current_a: float = math.nan
    self_test: str | None = None
    faults: list[str] = field(default_factory=list)
    fw: str | None = None
    health: dict[str, int] | None = None

    @property
    def fault_bits(self) -> int:
        bits = 0
        for f in self.faults:
            bits |= NODE_FAULTS[f]
        return bits

    def to_record(self) -> dict[str, Any]:
        """Canonical, JSON-safe record (NaN -> None) stored and hashed."""
        def flist(v):
            return None if v is None else [finite_or_none(x) for x in v]
        return {
            "schema": SCHEMA_ID,
            "node_id": self.node_id,
            "boot_id": self.boot_id,
            "seq": self.seq,
            "t_start_ms": self.t_start_ms,
            "t_end_ms": self.t_end_ms,
            "window_ms": self.window_ms,
            "received_ms": self.received_ms,
            "source": self.source,
            "time_synced": self.time_synced,
            "sample_rate_hz": finite_or_none(self.sample_rate_hz),
            "samples_expected": self.samples_expected,
            "samples_valid": self.samples_valid,
            "vib_rms_g": finite_or_none(self.vib_rms_g),
            "vib_rms_axis_g": flist(self.vib_rms_axis_g),
            "vib_p2p_g": flist(self.vib_p2p_g),
            "band_rms_g": flist(self.band_rms_g),
            "bands_hz": None if self.bands_hz is None else [flist(b) for b in self.bands_hz],
            "temp_c": finite_or_none(self.temp_c),
            "current_present": self.current_present,
            "current_a": finite_or_none(self.current_a) if self.current_present else None,
            "self_test": self.self_test,
            "faults": list(self.faults),
            "fw": self.fw,
            "health": self.health,
        }

    @classmethod
    def from_record(cls, rec: dict[str, Any]) -> NormalizedWindow:
        """Rebuild a window from its stored canonical record (warm start, replay)."""
        def f(v):
            return math.nan if v is None else float(v)

        def fl(v):
            return None if v is None else [f(x) for x in v]
        return cls(
            node_id=rec["node_id"], boot_id=int(rec["boot_id"]), seq=int(rec["seq"]),
            t_end_ms=int(rec["t_end_ms"]), received_ms=int(rec["received_ms"]),
            source=rec["source"], t_start_ms=rec.get("t_start_ms"),
            window_ms=rec.get("window_ms"), time_synced=rec.get("time_synced"),
            sample_rate_hz=rec.get("sample_rate_hz"),
            samples_expected=rec.get("samples_expected"),
            samples_valid=rec.get("samples_valid"), vib_rms_g=f(rec.get("vib_rms_g")),
            vib_rms_axis_g=fl(rec.get("vib_rms_axis_g")), vib_p2p_g=fl(rec.get("vib_p2p_g")),
            band_rms_g=fl(rec.get("band_rms_g")),
            bands_hz=rec.get("bands_hz"), temp_c=f(rec.get("temp_c")),
            current_present=bool(rec.get("current_present")), current_a=f(rec.get("current_a")),
            self_test=rec.get("self_test"), faults=list(rec.get("faults") or []),
            fw=rec.get("fw"), health=rec.get("health"))

    def to_c(self) -> AlgaWindow:
        w = AlgaWindow()
        w.boot_id = self.boot_id
        w.sequence_id = self.seq
        w.timestamp_ms = self.t_end_ms
        w.received_ms = self.received_ms
        presence = HAS_RECEIVED
        w.vibration_rms_g = self.vib_rms_g
        w.temperature_c = self.temp_c
        if self.t_start_ms is not None and self.window_ms is not None:
            w.t_start_ms = self.t_start_ms
            w.window_ms = self.window_ms
            presence |= HAS_TIMING
        if self.samples_expected is not None and self.samples_valid is not None:
            w.samples_expected = self.samples_expected
            w.samples_valid = self.samples_valid
            presence |= HAS_SAMPLE_COUNTS
        if self.vib_rms_axis_g is not None:
            for i, v in enumerate(self.vib_rms_axis_g):
                w.vib_rms_axis_g[i] = v
            presence |= HAS_AXES
        if self.vib_p2p_g is not None:
            for i, v in enumerate(self.vib_p2p_g):
                w.vib_p2p_axis_g[i] = v
            presence |= HAS_P2P
        if self.band_rms_g is not None:
            w.n_bands = len(self.band_rms_g)
            for i, v in enumerate(self.band_rms_g):
                w.band_rms_g[i] = v
            presence |= HAS_BANDS
        if self.current_present:
            w.current_a = self.current_a
            presence |= HAS_CURRENT
        if self.self_test is not None:
            w.self_test = SELF_TEST[self.self_test]
            presence |= HAS_SELF_TEST
        if self.time_synced is not None:
            w.time_synced = 1 if self.time_synced else 0
            presence |= HAS_TIME_SYNC
        w.node_fault_flags = self.fault_bits
        presence |= HAS_NODE_FAULTS
        w.presence = presence
        return w


def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def _int(d: dict, key: str, lo: int, hi: int, node: str | None) -> int:
    v = d[key]
    if not _is_int(v) or v < lo or v > hi:
        raise PayloadError(f"field '{key}' must be an integer in [{lo}, {hi}]", node)
    return v


def _measurement(v: Any, key: str, node: str | None) -> float:
    """number | null | NaN/Infinity token -> float (NaN for null)."""
    if v is None:
        return math.nan
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        raise PayloadError(f"field '{key}' must be a number or null", node)
    return float(v)


def _triple(v: Any, key: str, node: str | None) -> list[float]:
    if not isinstance(v, list) or len(v) != 3:
        raise PayloadError(f"field '{key}' must be a list of 3 numbers", node)
    return [_measurement(x, key, node) for x in v]


def parse_telemetry(raw: bytes, received_ms: int, topic_node_id: str | None = None
                    ) -> NormalizedWindow:
    node = topic_node_id
    if len(raw) > MAX_PAYLOAD_BYTES:
        raise PayloadError(f"payload exceeds {MAX_PAYLOAD_BYTES} bytes", node)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise PayloadError("payload is not valid UTF-8", node) from None
    try:
        doc = json.loads(text)
    except ValueError as exc:
        raise PayloadError(f"payload is not valid JSON ({exc.msg})", node) from None
    if not isinstance(doc, dict):
        raise PayloadError("payload must be a JSON object", node)

    nid = doc.get("node_id")
    if isinstance(nid, str) and NODE_ID_RE.match(nid):
        node = node or nid
    unknown = set(doc) - TOP_KEYS
    if unknown:
        raise PayloadError(f"unexpected field(s): {', '.join(sorted(unknown))}", node)
    missing = [k for k in REQUIRED if k not in doc]
    if missing:
        raise PayloadError(f"missing required field(s): {', '.join(missing)}", node)
    if doc["schema"] != SCHEMA_ID:
        raise PayloadError(f"unsupported schema {doc['schema']!r} (expected {SCHEMA_ID})", node)
    if not isinstance(nid, str) or not NODE_ID_RE.match(nid):
        raise PayloadError("field 'node_id' is invalid", node)
    if topic_node_id is not None and nid != topic_node_id:
        raise PayloadError(f"node_id {nid!r} does not match topic node {topic_node_id!r}", node)
    if doc["source"] not in SOURCES:
        raise PayloadError(f"field 'source' must be one of {SOURCES}", node)

    w = NormalizedWindow(
        node_id=nid,
        boot_id=_int(doc, "boot_id", 0, U32_MAX, node),
        seq=_int(doc, "seq", 0, U32_MAX, node),
        t_end_ms=_int(doc, "t_end_ms", 0, U63_MAX, node),
        received_ms=int(received_ms),
        source=doc["source"],
    )
    if "t_start_ms" in doc or "window_ms" in doc:
        if "t_start_ms" not in doc or "window_ms" not in doc:
            raise PayloadError("t_start_ms and window_ms must be given together", node)
        w.t_start_ms = _int(doc, "t_start_ms", 0, U63_MAX, node)
        w.window_ms = _int(doc, "window_ms", 1, 3_600_000, node)
    if "time_synced" in doc:
        if not isinstance(doc["time_synced"], bool):
            raise PayloadError("field 'time_synced' must be a boolean", node)
        w.time_synced = doc["time_synced"]
    if "sample_rate_hz" in doc:
        sr = doc["sample_rate_hz"]
        if isinstance(sr, bool) or not isinstance(sr, (int, float)) or not (0 < sr <= 100000):
            raise PayloadError("field 'sample_rate_hz' must be a positive number", node)
        w.sample_rate_hz = float(sr)
    if "samples_expected" in doc or "samples_valid" in doc:
        if "samples_expected" not in doc or "samples_valid" not in doc:
            raise PayloadError("samples_expected and samples_valid must be given together", node)
        w.samples_expected = _int(doc, "samples_expected", 0, 1_000_000, node)
        w.samples_valid = _int(doc, "samples_valid", 0, 1_000_000, node)

    vib = doc["vib"]
    if not isinstance(vib, dict):
        raise PayloadError("field 'vib' must be an object", node)
    if set(vib) - VIB_KEYS:
        raise PayloadError(f"unexpected vib field(s): {', '.join(sorted(set(vib) - VIB_KEYS))}",
                           node)
    if "rms_g" not in vib:
        raise PayloadError("missing required field: vib.rms_g", node)
    w.vib_rms_g = _measurement(vib["rms_g"], "vib.rms_g", node)
    if "rms_axis_g" in vib:
        w.vib_rms_axis_g = _triple(vib["rms_axis_g"], "vib.rms_axis_g", node)
    if "p2p_g" in vib:
        w.vib_p2p_g = _triple(vib["p2p_g"], "vib.p2p_g", node)
    if "band_rms_g" in vib or "bands_hz" in vib:
        bands, edges = vib.get("band_rms_g"), vib.get("bands_hz")
        if (not isinstance(bands, list) or not isinstance(edges, list) or
                not 1 <= len(bands) <= MAX_BANDS or len(bands) != len(edges)):
            raise PayloadError("vib.band_rms_g and vib.bands_hz must be equal-length lists "
                               f"of 1..{MAX_BANDS} entries", node)
        w.band_rms_g = [_measurement(x, "vib.band_rms_g", node) for x in bands]
        w.bands_hz = []
        for e in edges:
            if (not isinstance(e, list) or len(e) != 2 or
                    not all(isinstance(x, (int, float)) and not isinstance(x, bool) for x in e)
                    or not 0 < e[0] < e[1]):
                raise PayloadError("vib.bands_hz entries must be [low, high] with 0 < low < high",
                                   node)
            w.bands_hz.append([float(e[0]), float(e[1])])

    w.temp_c = _measurement(doc["temp_c"], "temp_c", node)
    if "current_a" in doc:
        w.current_present = True
        w.current_a = _measurement(doc["current_a"], "current_a", node)
    if "self_test" in doc:
        if doc["self_test"] not in SELF_TEST:
            raise PayloadError(f"field 'self_test' must be one of {tuple(SELF_TEST)}", node)
        w.self_test = doc["self_test"]
    if "faults" in doc:
        faults = doc["faults"]
        if not isinstance(faults, list) or any(f not in NODE_FAULTS for f in faults):
            raise PayloadError(f"field 'faults' must list known fault names {tuple(NODE_FAULTS)}",
                               node)
        w.faults = sorted(set(faults))
    if "fw" in doc:
        if not isinstance(doc["fw"], str) or not 0 < len(doc["fw"]) <= 32:
            raise PayloadError("field 'fw' must be a string of at most 32 characters", node)
        w.fw = doc["fw"]
    if "health" in doc:
        h = doc["health"]
        if not isinstance(h, dict) or set(h) - HEALTH_KEYS:
            raise PayloadError("field 'health' must be an object with known counters", node)
        for k, v in h.items():
            lo = -200 if k == "rssi_dbm" else 0
            if not _is_int(v) or not lo <= v <= U32_MAX:
                raise PayloadError(f"health.{k} must be an integer", node)
        w.health = dict(sorted(h.items()))
    return w
