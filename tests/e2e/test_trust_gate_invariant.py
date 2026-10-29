"""The trust gate on everything the live run stored: equipment health is only
judged on trusted data, and sensor or transport faults are never reported as
equipment problems.  Ground truth comes from the simulator labels, which were
kept outside the payloads."""

import json

import jsonschema
from e2e_support import ROOT, SCENARIOS

SENSORISH = ("sensor_fault", "transport_fault")


def test_check_equipment_only_on_trusted_windows(system):
    for d in system.decisions():
        if d["outcome"] == "CHECK_EQUIPMENT":
            assert d["kind"] == "window" and d["trust"]["trusted"], d["reason"]
            assert not d["trust"]["blocking"], d["reason"]


def test_untrusted_or_blocked_data_is_always_check_sensor(system):
    for d in system.decisions():
        if not d["trust"]["trusted"] or d["trust"]["blocking"]:
            assert d["outcome"] == "CHECK_SENSOR", (d["node_id"], d["seq"], d["reason"])


def test_no_check_equipment_on_sensor_or_transport_faults(system):
    checked = 0
    for node in SCENARIOS:
        labels = system.labels(node)
        for d in system.decisions(node):
            if d["outcome"] != "CHECK_EQUIPMENT":
                continue
            labs = labels.get((d["boot_id"], d["seq"], d["timestamp_ms"]))
            assert labs, f"{node} seq {d['seq']}: decision without a published window"
            assert not any(lab.truth in SENSORISH for lab in labs), (node, d["seq"], labs)
            checked += 1
    assert checked > 0, "the run must contain equipment decisions to check"


def test_sensor_and_transport_faults_are_check_sensor(system):
    checked = 0
    for node in SCENARIOS:
        labels = system.labels(node)
        for d in system.decisions(node):
            labs = labels.get((d["boot_id"], d["seq"], d["timestamp_ms"])) \
                if d["kind"] == "window" else None
            if labs and all(lab.truth in SENSORISH and "CHECK_SENSOR" in lab.expected
                            for lab in labs):
                assert d["outcome"] == "CHECK_SENSOR", (node, d["seq"], labs, d["reason"])
                checked += 1
    assert checked > 20


def test_payloads_and_records_carry_no_ground_truth(system):
    schema = json.loads((ROOT / "schemas" / "telemetry_window.v1.schema.json").read_text())
    allowed = set(schema["properties"])
    for node in SCENARIOS:
        for e in system.events[node]:
            if e.kind != "telemetry":
                continue
            try:
                doc = json.loads(e.payload)
            except ValueError:
                continue              # the deliberately truncated JSON
            assert set(doc) <= allowed, set(doc) - allowed
    with system.db() as c:
        for (record,) in c.execute("SELECT record_json FROM windows"):
            keys = set(json.loads(record))
            assert not keys & {"truth", "label", "fault", "expected"}, keys


def test_stored_decisions_conform_to_the_published_schema(system):
    schema = json.loads((ROOT / "schemas" / "diagnostic_event.v1.schema.json").read_text())
    validator = jsonschema.Draft202012Validator(schema)
    for d in system.decisions():
        doc = {k: v for k, v in d.items() if k not in ("id", "window_id")}
        errors = [e.message for e in validator.iter_errors(doc)]
        assert not errors, (d["node_id"], d["seq"], errors[:3])
