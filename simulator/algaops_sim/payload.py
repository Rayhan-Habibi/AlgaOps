"""telemetry_window.v1 / node_status.v1 payloads, byte-compatible with the
ESP32 firmware (firmware/.../telemetry_payload.c): same keys, same rounding,
`source` = "simulator" so the dashboard shows DEMO / SYNTHETIC."""

from __future__ import annotations

import json
import math
from typing import Any

from .scenario import Scenario
from .signal_model import WindowSample

FW_VERSION = "sim-2.0.0"


def window_doc(sc: Scenario, w: WindowSample, source: str = "simulator") -> dict[str, Any]:
    axes = [round(a, 5) for a in w.vib_axes]
    rms = round(math.sqrt(sum(a * a for a in axes)), 5)
    doc: dict[str, Any] = {
        "schema": "algaops.telemetry_window.v1",
        "node_id": sc.node_id,
        "boot_id": w.boot_id,
        "seq": w.seq,
        "t_start_ms": w.t_start_ms,
        "t_end_ms": w.t_end_ms,
        "window_ms": w.window_ms,
        "time_synced": w.time_synced,
        "sample_rate_hz": sc.sample_rate_hz,
        "samples_expected": w.samples_expected,
        "samples_valid": w.samples_valid,
        "vib": {"rms_g": rms, "rms_axis_g": axes, "p2p_g": [round(p, 5) for p in w.vib_p2p]},
        "temp_c": None if w.temp_c is None else round(w.temp_c, 4),
        "self_test": w.self_test,
        "faults": list(w.faults),
        "fw": FW_VERSION,
        "source": source,
    }
    if w.current_a is not None:
        doc["current_a"] = w.current_a
    if "vib" in w.null_fields:
        doc["vib"]["rms_g"] = None
    if "temp" in w.null_fields:
        doc["temp_c"] = None
    if "current" in w.null_fields and "current_a" in doc:
        doc["current_a"] = None
    return doc


def build_payload(sc: Scenario, w: WindowSample, source: str = "simulator") -> bytes:
    return json.dumps(window_doc(sc, w, source), separators=(",", ":")).encode("utf-8")


def status_payload(sc: Scenario, state: str, boot_id: int) -> bytes:
    doc = {"schema": "algaops.node_status.v1", "node_id": sc.node_id, "state": state,
           "boot_id": boot_id, "fw": FW_VERSION, "source": "simulator",
           "sensors": {"adxl345": True, "ds18b20": True, "current": sc.current_sensor}}
    return json.dumps(doc, separators=(",", ":")).encode("utf-8")
