"""Pipeline behaviour over simulator scenarios: invariants, alerts, timeouts,
atomic storage and warm start."""

import json

import pytest
from algaops_sim.replay import generate_stream
from algaops_sim.scenario import load_scenario
from gwtest import TOPIC, feed, last_decision, make_doc

from algaops_gateway.analytics.evaluate import build_baselines, run_scenario
from algaops_gateway.audit.checkpoint import CheckpointWriter
from algaops_gateway.audit.verify import verify_database
from algaops_gateway.pipeline import Pipeline
from algaops_gateway.storage.database import Database


@pytest.fixture(scope="module")
def baselines(lib, policy):
    return build_baselines(lib, policy, "baseline_modes")


SCENARIOS = ["normal_steady", "stuck_vibration", "missing_packet", "duplicate_sequence",
             "out_of_order_sequence", "bad_timestamp", "stale_late_delivery",
             "out_of_range_spike", "non_finite_values", "malformed_payload", "self_test_failure",
             "sensor_disagreement", "equipment_vibration_anomaly", "equipment_thermal_anomaly",
             "suspicious_plus_blocking", "disconnect_reconnect_replay", "mode_transitions"]


@pytest.mark.parametrize("name", SCENARIOS)
def test_scenario_invariants(lib, policy, baselines, name):
    r = run_scenario(lib, policy, name, baselines, keep_timeline=True)
    # the decisive invariant: no equipment alarm from untrusted or faulty data
    assert r["untrusted_check_equipment"] == 0
    assert r["attribution_errors"] == 0
    for t in r["timeline"]:
        if t["outcome"] == "CHECK_EQUIPMENT":
            assert t["truth"] in ("equipment_fault", "normal"), t   # never a sensor fault
    normal = r["truth_vs_outcome"].get("normal", {})
    assert normal.get("CHECK_SENSOR", 0) == 0, "healthy data must not be reported as a sensor issue"
    # a 3-sigma z-score has a small per-window false-positive rate on Gaussian noise; it is
    # measured and reported, and alert confirmation keeps it from raising alerts
    n_normal = sum(normal.values())
    assert normal.get("CHECK_EQUIPMENT", 0) <= max(2, 0.02 * n_normal)
    if not any(t["truth"] == "equipment_fault" for t in r["timeline"]):
        assert "CHECK_EQUIPMENT" not in r["alerts_opened"]


def test_three_outcomes_are_demonstrated(lib, policy, baselines):
    seen = set()
    for name in ("normal_steady", "stuck_vibration", "equipment_vibration_anomaly"):
        seen |= set(run_scenario(lib, policy, name, baselines, False)["outcome_counts"])
    assert {"NORMAL", "CHECK_SENSOR", "CHECK_EQUIPMENT"} <= seen


def test_fault_scenarios_are_detected(lib, policy, baselines):
    expect_detected = {"stuck_vibration", "duplicate_sequence", "out_of_order_sequence",
                       "bad_timestamp", "stale_late_delivery", "out_of_range_spike",
                       "non_finite_values", "malformed_payload", "self_test_failure",
                       "sensor_disagreement", "equipment_vibration_anomaly",
                       "equipment_thermal_anomaly"}
    for name in expect_detected:
        r = run_scenario(lib, policy, name, baselines, False)
        assert r["fault_events"], name
        assert all(e["detected"] for e in r["fault_events"]), (name, r["fault_events"])


def test_stream_timeout_never_leaves_normal_showing(pipeline, clock):
    feed(pipeline, clock, 5)
    assert pipeline.tick(clock.t + 5000) == 0
    assert pipeline.tick(clock.t + 20_000) == 1
    d = last_decision(pipeline)
    assert d["kind"] == "stream_timeout" and d["outcome"] == "CHECK_SENSOR"
    # The timeout is persistent by construction, so it opens the alert even
    # though the policy needs 3 confirming windows for window-level findings.
    rows = pipeline.db.writer.execute("SELECT kind, status FROM alerts").fetchall()
    assert [(r[0], r[1]) for r in rows] == [("CHECK_SENSOR", "open")]
    assert pipeline.tick(clock.t + 40_000) == 0          # once per silence period
    feed(pipeline, clock, 8, start_seq=6)                # data resumes and stays clean
    rows = pipeline.db.writer.execute("SELECT status FROM alerts").fetchall()
    assert [r[0] for r in rows] == ["cleared"]


def test_restart_keeps_timeout_alert_consistent(lib, policy, tmp_path, clock):
    """Window replay alone cannot rebuild an alert opened by a stream timeout: the
    warm start must restore it, so it still clears and is not reported twice."""
    db = Database(tmp_path / "restart.db")

    def start():
        p = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw", clock=clock,
                     checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"))
        p.open()
        return p

    p = start()
    feed(p, clock, 5)
    assert p.tick(clock.t + 20_000) == 1
    p.close()
    p = start()                                          # restart while the node is silent
    assert p.tick(clock.t + 30_000) == 0, "the same silence is not reported twice"
    feed(p, clock, 8, start_seq=6)
    rows = db.writer.execute("SELECT kind, status FROM alerts ORDER BY id").fetchall()
    assert [(r[0], r[1]) for r in rows] == [("CHECK_SENSOR", "cleared")]
    p.close()
    db.close()


def test_rejected_message_on_node_topic_creates_check_sensor(pipeline, clock):
    feed(pipeline, clock, 3)
    pipeline.handle_message(TOPIC, b'{"schema": "nope"}', clock.t + 100)
    d = last_decision(pipeline)
    assert d["kind"] == "malformed" and d["outcome"] == "CHECK_SENSOR"
    pipeline.handle_message("algaops/v1/nodes/bad+id/telemetry", b"{}", clock.t + 200)
    log = [r[0] for r in pipeline.db.writer.execute("SELECT status FROM ingest_log")]
    assert log.count("rejected") == 2


def test_storage_failure_is_atomic_and_not_acknowledged(pipeline, clock, monkeypatch):
    feed(pipeline, clock, 3)
    counts = lambda: [pipeline.db.writer.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0]  # noqa
                      for t in ("windows", "diagnostics", "audit_events")]
    before = counts()
    from algaops_gateway.storage import repositories as repo
    real = repo.insert_diagnostic

    def boom(*a, **k):
        real(*a, **k)
        raise OSError("disk full (simulated)")
    monkeypatch.setattr(repo, "insert_diagnostic", boom)
    ok = pipeline.handle_message(TOPIC, json.dumps(make_doc(4)).encode(), clock.t + 1280)
    assert ok is False, "must not be acknowledged -> broker redelivers"
    assert counts() == before, "no partial rows after a failed transaction"
    monkeypatch.setattr(repo, "insert_diagnostic", real)
    # redelivery after recovery is processed normally (engine state was rebuilt)
    assert pipeline.handle_message(TOPIC, json.dumps(make_doc(4)).encode(), clock.t + 1300)
    d = last_decision(pipeline)
    assert "DUPLICATE_SEQ" not in d["trust"]["flags"] and d["seq"] == 4
    assert verify_database(pipeline.db.writer, pipeline.hasher).ok


def test_warm_start_restores_sequence_state(lib, policy, db, clock, tmp_path):
    p1 = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw", clock=clock,
                  checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"))
    p1.open()
    feed(p1, clock, 20)
    p1.close()
    p2 = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw", clock=clock,
                  checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"))
    p2.open()
    # exact redelivery of the last pre-restart window: idempotent
    p2.handle_message(TOPIC, json.dumps(make_doc(20)).encode(), clock.t + 10)
    assert p2.stats.duplicate_deliveries == 1
    # next window continues the sequence without a false gap
    feed(p2, clock, 1, start_seq=21)
    d = last_decision(p2)
    assert d["trust"]["flags"] == [] and d["outcome"] in ("NORMAL", "NOT_EVALUATED")
    # an old sequence number after restart is still detected
    old = make_doc(10) | {"t_end_ms": clock.t + 500, "t_start_ms": clock.t - 780}
    p2.handle_message(TOPIC, json.dumps(old).encode(), clock.t + 600)
    assert "OUT_OF_ORDER_SEQ" in last_decision(p2)["trust"]["flags"]
    rep = verify_database(db.writer, p2.hasher, tmp_path / "cp.jsonl")
    p2.close()
    assert rep.ok, rep.issues


def test_labels_never_reach_the_pipeline():
    sc = load_scenario("malformed_payload")
    events, _ = generate_stream(sc)
    for e in events:
        text = e.payload.decode("utf-8", "replace")
        for word in ("sensor_fault", "transport_fault", '"truth"', '"expected"'):
            assert word not in text
    leaked = [e for e in events if b'"label"' in e.payload]
    assert len(leaked) == 1, "only the deliberately malformed message carries a label field"
