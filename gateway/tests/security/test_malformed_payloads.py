"""Hostile / malformed input never crashes the pipeline, is always recorded, and
never produces an equipment diagnosis."""

import json
import random

from gwtest import TOPIC, feed, make_doc

from algaops_gateway.audit.verify import verify_database


def test_random_bytes_and_structures(pipeline, clock):
    feed(pipeline, clock, 25)
    rng = random.Random(7)
    samples = [bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 300))) for _ in range(40)]
    samples += [b"", b"null", b"[]", b'"x"', b"{" * 5000, b"9" * 9000,
                json.dumps({"a": [[[[[[[[1]]]]]]]]}).encode(),
                json.dumps(make_doc(26) | {"vib": "loud"}).encode(),
                json.dumps(make_doc(26) | {"temp_c": "31"}).encode(),
                json.dumps(make_doc(26) | {"boot_id": 1.5}).encode(),
                json.dumps(make_doc(26) | {"seq": 10**30}).encode()]
    for i, raw in enumerate(samples):
        clock.t += 10
        assert pipeline.handle_message(TOPIC, raw, clock.t) is True
    rows = pipeline.db.writer.execute("SELECT outcome, kind FROM diagnostics WHERE kind = "
                                      "'malformed'").fetchall()
    assert len(rows) == len(samples)
    assert {r["outcome"] for r in rows} == {"CHECK_SENSOR"}
    assert pipeline.db.writer.execute("SELECT COUNT(*) FROM diagnostics WHERE outcome = "
                                      "'CHECK_EQUIPMENT'").fetchone()[0] == 0
    assert verify_database(pipeline.db.writer, pipeline.hasher).ok


def test_topic_injection_and_identity_mismatch(pipeline, clock):
    bad_topics = ["algaops/v1/nodes/../telemetry", "algaops/v1/nodes/a b/telemetry",
                  "algaops/v1/nodes/x/command", "algaops/v2/nodes/x/telemetry",
                  "algaops/v1/nodes/" + "x" * 80 + "/telemetry"]
    for t in bad_topics:
        assert pipeline.handle_message(t, json.dumps(make_doc(1)).encode(), clock.t)
    # payload claims another node than the topic: rejected, attributed to the topic node
    spoof = json.dumps(make_doc(1) | {"node_id": "node-b"}).encode()
    pipeline.handle_message(TOPIC, spoof, clock.t)
    log = [dict(r) for r in pipeline.db.writer.execute("SELECT node_id, status, reason FROM "
                                                       "ingest_log")]
    assert len(log) == len(bad_topics) + 1 and all(r["status"] == "rejected" for r in log)
    assert log[-1]["node_id"] == "node-a" and "does not match" in log[-1]["reason"]
    assert pipeline.db.writer.execute("SELECT COUNT(*) FROM windows").fetchone()[0] == 0


def test_leaked_ground_truth_field_is_rejected(pipeline, clock):
    feed(pipeline, clock, 3)
    leaked = json.dumps(make_doc(4) | {"label": "equipment_fault"}).encode()
    pipeline.handle_message(TOPIC, leaked, clock.t + 1280)
    r = pipeline.db.writer.execute("SELECT reason FROM ingest_log").fetchone()
    assert "unexpected field" in r["reason"]


def test_oversized_payload_rejected_quickly(pipeline, clock):
    big = json.dumps(make_doc(1) | {"fw": "x" * 20000}).encode()
    pipeline.handle_message(TOPIC, big, clock.t)
    r = pipeline.db.writer.execute("SELECT reason, raw_excerpt FROM ingest_log").fetchone()
    assert "exceeds" in r["reason"] and len(r["raw_excerpt"]) <= 200
