import json

import pytest

from algaops_gateway.ingest.node_status import parse_status
from algaops_gateway.ingest.payload_schema import PayloadError

ST = "algaops/v1/nodes/node-a/status"


def doc(**over):
    d = {"schema": "algaops.node_status.v1", "node_id": "node-a", "state": "online",
         "boot_id": 7, "fw": "2.0.0", "rssi_dbm": -60, "queue_depth": 0, "source": "hardware",
         "sensors": {"adxl345": True, "ds18b20": True, "current": False}}
    d.update(over)
    return json.dumps(d).encode()


def test_valid_status():
    s = parse_status(doc(), 5, "node-a")
    assert s.state == "online" and s.fields["boot_id"] == 7 and s.to_record()["received_ms"] == 5


@pytest.mark.parametrize("raw", [doc(state="exploded"), doc(node_id="node-b"), doc(extra=1),
                                 doc(schema="x"), doc(boot_id="7"), b"{", doc(sensors={"a": 1})])
def test_invalid_status(raw):
    with pytest.raises(PayloadError):
        parse_status(raw, 5, "node-a")


def test_status_changes_are_audited(pipeline):
    pipeline.handle_message(ST, doc(), 1000)
    pipeline.handle_message(ST, doc(), 2000)                     # no change: no new event
    pipeline.handle_message(ST, doc(state="offline"), 3000)      # LWT
    events = [r[0] for r in pipeline.db.writer.execute(
        "SELECT payload FROM audit_events WHERE type = 'node.status' ORDER BY seq")]
    states = [json.loads(e)["state"] for e in events]
    assert states == ["online", "offline"]
    # a status message never creates a diagnostic decision
    assert pipeline.db.writer.execute("SELECT COUNT(*) FROM diagnostics").fetchone()[0] == 0
