"""Export a scenario's received telemetry stream as a v2 CSV for the C CLI
(algaops --config gateway/config/policy.default.toml --input ...).

Rows follow delivery order, exact MQTT redeliveries are omitted (the gateway
drops them idempotently) and malformed messages become unparseable rows, so
the CLI sees the same sequence of inputs as the gateway pipeline.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

from .faults import PublishEvent

NODE_FAULT_BITS = {"ACCEL_BUS": 1, "ACCEL_OVERRUN": 2, "ACCEL_MISSING": 4, "TEMP_CRC": 8,
                   "TEMP_MISSING": 16, "TEMP_POR": 32, "CURRENT_ADC": 64, "CURRENT_CLIP": 128}

COLUMNS = ["timestamp_ms", "sequence_id", "vibration_rms_g", "temperature_c", "boot_id",
           "received_ms", "t_start_ms", "window_ms", "samples_expected", "samples_valid",
           "vib_rms_x_g", "vib_rms_y_g", "vib_rms_z_g", "vib_p2p_x_g", "vib_p2p_y_g",
           "vib_p2p_z_g", "current_a", "self_test", "time_synced", "node_fault_flags"]


def _num(v) -> str:
    if v is None:
        return "nan"
    if isinstance(v, float) and not math.isfinite(v):
        return "nan"
    return repr(v) if isinstance(v, float) else str(v)


def event_row(e: PublishEvent) -> list[str]:
    malformed = e.label is not None and e.label.fault == "malformed"
    try:
        d = json.loads(e.payload)
    except ValueError:
        malformed = True
    if malformed:
        return ["MALFORMED"] + [""] * (len(COLUMNS) - 1)
    vib = d["vib"]
    axes = vib.get("rms_axis_g") or [None, None, None]
    p2p = vib.get("p2p_g") or [None, None, None]
    bits = sum(NODE_FAULT_BITS[f] for f in d.get("faults", []))
    return [str(d["t_end_ms"]), str(d["seq"]), _num(vib.get("rms_g")), _num(d.get("temp_c")),
            str(d["boot_id"]), str(e.t_ms), str(d.get("t_start_ms", "")),
            str(d.get("window_ms", "")), str(d.get("samples_expected", "")),
            str(d.get("samples_valid", "")), *(_num(x) for x in axes), *(_num(x) for x in p2p),
            _num(d["current_a"]) if "current_a" in d else "",
            d.get("self_test", ""), ("1" if d.get("time_synced") else "0")
            if "time_synced" in d else "", str(bits)]


def export_csv(events: list[PublishEvent], path: Path, title: str, description: str) -> int:
    rows = [event_row(e) for e in events if e.kind == "telemetry" and not e.redelivery]
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(f"# AlgaOps Assurance - Synthetic Scenario (v2 schema): {title}\n")
        fh.write("# SYNTHETIC DATA: generated deterministically by the AlgaOps simulator\n")
        fh.write("# (python -m algaops_sim export-csv). Not measurements from any PBR.\n")
        for line in description.strip().splitlines():
            fh.write(f"# {line}\n")
        fh.write(",".join(COLUMNS) + "\n")
        for r in rows:
            fh.write(",".join(r) + "\n")
    return len(rows)
