"""Every simulator payload conforms to the published JSON schemas and to the
gateway's strict parser; labels never appear in payloads."""

import json
from pathlib import Path

import jsonschema
import pytest

from algaops_sim.replay import generate_stream
from algaops_sim.scenario import list_scenarios

ROOT = Path(__file__).resolve().parents[2]
SCHEMAS = ROOT / "schemas"


def schema(name):
    return json.loads((SCHEMAS / name).read_text(encoding="utf-8"))


TELEMETRY = schema("telemetry_window.v1.schema.json")
STATUS = schema("node_status.v1.schema.json")


@pytest.mark.parametrize("sc", [s for s in list_scenarios() if s.name != "normal_8h_replay"],
                         ids=lambda s: s.name)
def test_payloads_validate(sc):
    from algaops_gateway.ingest.payload_schema import parse_telemetry
    events, _ = generate_stream(sc)
    for e in events:
        if e.kind == "status":
            jsonschema.validate(json.loads(e.payload), STATUS)
            continue
        if e.label and e.label.fault == "malformed":
            continue
        doc = json.loads(e.payload)
        jsonschema.validate(doc, TELEMETRY)
        parse_telemetry(e.payload, e.t_ms, sc.node_id)      # gateway accepts it
        assert doc["source"] == "simulator"
        assert not {"label", "truth", "expected", "fault"} & set(doc)


def test_malformed_variants_fail_schema():
    sc = next(s for s in list_scenarios() if s.name == "malformed_payload")
    events, _ = generate_stream(sc)
    bad = [e for e in events if e.label and e.label.fault == "malformed"]
    assert len(bad) == 5
    for e in bad:
        try:
            jsonschema.validate(json.loads(e.payload), TELEMETRY)
            ok = True
        except (ValueError, jsonschema.ValidationError):
            ok = False
        assert not ok
