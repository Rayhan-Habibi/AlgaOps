"""The firmware's own JSON (written by the host-built test_telemetry_payload
binary) against the published schemas and the gateway's strict parsers, so the
node and the gateway cannot drift apart silently."""

import json
import math
import subprocess
from pathlib import Path

import jsonschema
import pytest

from algaops_gateway.ingest.node_status import parse_status
from algaops_gateway.ingest.payload_schema import parse_telemetry

ROOT = Path(__file__).resolve().parents[3]
BUILD = ROOT / "firmware" / "esp32-sensor-node" / "host_test" / "build"
NODE = "esp32-a1b2c3d4e5f6"


@pytest.fixture(scope="module")
def samples(tmp_path_factory):
    exe = next((p for p in (BUILD / "test_telemetry_payload.exe", BUILD / "test_telemetry_payload")
                if p.is_file()), None)
    if exe is None:
        pytest.skip("firmware host tests not built (see firmware/esp32-sensor-node/host_test)")
    out = tmp_path_factory.mktemp("fw_payloads")
    r = subprocess.run([str(exe), "--emit", str(out)], capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return {p.name: p.read_bytes() for p in out.glob("*.json")}


def validator(name):
    schema = json.loads((ROOT / "schemas" / name).read_text(encoding="utf-8"))
    return jsonschema.Draft202012Validator(schema)


def test_samples_were_written(samples):
    assert set(samples) == {"telemetry_full.json", "telemetry_all_faults.json",
                            "telemetry_nulls_unsynced.json", "status_online.json",
                            "status_lwt.json"}


def test_telemetry_matches_the_schema_and_the_gateway_parser(samples):
    v = validator("telemetry_window.v1.schema.json")
    for name, raw in samples.items():
        if not name.startswith("telemetry"):
            continue
        doc = json.loads(raw)
        errors = [e.message for e in v.iter_errors(doc)]
        assert not errors, (name, errors)
        w = parse_telemetry(raw, 1793000061000, NODE)       # raises on any contract breach
        assert w.node_id == NODE and w.source == "hardware" and w.seq == 42


def test_parsed_values_keep_their_meaning(samples):
    full = parse_telemetry(samples["telemetry_full.json"], 1793000061000, NODE)
    assert full.time_synced and full.t_end_ms - full.t_start_ms == full.window_ms == 1280
    assert math.isclose(full.vib_rms_g, 0.42, abs_tol=1e-5)
    assert math.isclose(full.temp_c, 31.0625) and not full.current_present
    assert full.self_test == "pass" and full.faults == []

    faulty = parse_telemetry(samples["telemetry_all_faults.json"], 1793000061000, NODE)
    assert sorted(faulty.faults) == sorted(["ACCEL_BUS", "ACCEL_OVERRUN", "ACCEL_MISSING",
                                            "TEMP_CRC", "TEMP_MISSING", "TEMP_POR",
                                            "CURRENT_ADC", "CURRENT_CLIP"])
    assert faulty.fault_bits == 0xFF and faulty.self_test == "fail"
    assert faulty.current_present and math.isclose(faulty.current_a, 1.804, abs_tol=1e-6)
    assert faulty.samples_valid == 1023 < faulty.samples_expected

    nulls = parse_telemetry(samples["telemetry_nulls_unsynced.json"], 1793000061000, NODE)
    assert math.isnan(nulls.vib_rms_g) and math.isnan(nulls.temp_c)
    assert nulls.current_present and math.isnan(nulls.current_a)
    assert nulls.time_synced is False and nulls.t_end_ms == 61280


def test_status_messages_match_the_schema_and_parser(samples):
    v = validator("node_status.v1.schema.json")
    for name in ("status_online.json", "status_lwt.json"):
        raw = samples[name]
        assert not [e.message for e in v.iter_errors(json.loads(raw))], name
        st = parse_status(raw, 1793000061000, NODE)
        assert st.state == ("online" if "online" in name else "offline")
