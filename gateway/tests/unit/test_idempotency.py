import json

from gwtest import TOPIC, feed, last_decision, make_doc

from algaops_gateway.ingest.idempotency import IdempotencyGuard


def test_guard_lru():
    g = IdempotencyGuard(capacity=2)
    g.remember("n", 1, 1, "a")
    assert g.is_duplicate("n", 1, 1, "a")
    assert not g.is_duplicate("n", 1, 1, "b")       # same key, different content
    g.remember("n", 1, 2, "c")
    g.remember("n", 1, 3, "d")
    assert not g.is_duplicate("n", 1, 1, "a")       # evicted
    assert g.duplicates == 1


def test_exact_redelivery_dropped_conflict_flagged(pipeline, clock):
    feed(pipeline, clock, 3)
    raw = json.dumps(make_doc(3)).encode()
    before = pipeline.db.writer.execute("SELECT COUNT(*) FROM diagnostics").fetchone()[0]
    pipeline.handle_message(TOPIC, raw, clock.t + 10)          # byte-identical retry
    after = pipeline.db.writer.execute("SELECT COUNT(*) FROM diagnostics").fetchone()[0]
    assert after == before and pipeline.stats.duplicate_deliveries == 1
    log = pipeline.db.writer.execute("SELECT status FROM ingest_log").fetchall()
    assert [r[0] for r in log] == ["duplicate_delivery"]
    # same sequence number, different content: a real integrity problem
    conflict = json.dumps(make_doc(3, temp_c=31.5) | {"t_end_ms": make_doc(3)["t_end_ms"] + 300,
                                                       "t_start_ms": make_doc(3)["t_start_ms"] + 300}
                          ).encode()
    pipeline.handle_message(TOPIC, conflict, clock.t + 400)
    d = last_decision(pipeline)
    assert d["outcome"] == "CHECK_SENSOR" and "DUPLICATE_SEQ" in d["trust"]["flags"]


def test_guard_warm_from_db(pipeline, clock):
    feed(pipeline, clock, 4)
    g = IdempotencyGuard()
    g.warm(pipeline.db.writer)
    row = pipeline.db.writer.execute("SELECT boot_id, seq, payload_digest FROM windows "
                                     "ORDER BY id LIMIT 1").fetchone()
    assert g.is_duplicate("node-a", row[0], row[1], row[2])
