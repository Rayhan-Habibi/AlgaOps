"""Baseline capture from trusted windows, activation, and effect on decisions."""

import json

from gwtest import feed, last_decision

from algaops_gateway.diagnostics.baseline_manager import (
    baseline_to_stats,
    mode_index,
    stats_to_baseline,
)


def test_stats_roundtrip(lib):
    from algaops_gateway.core.libalgaops import AlgaBaseline
    bl = AlgaBaseline()
    bl.mode, bl.n, bl.valid, bl.feature_mask = 3, 40, 1, 0b1001
    bl.mean[0], bl.std[0], bl.mean[3], bl.std[3] = 0.42, 0.006, 1.8, 0.02
    bl.cov[0], bl.cov[3], bl.cov[12], bl.cov[15] = 3.6e-5, 1e-5, 1e-5, 4e-4
    stats = baseline_to_stats(bl)
    assert stats["features"] == ["vib_rms_g", "current_a"] and stats["mode"] == "STEADY"
    back = stats_to_baseline(json.loads(json.dumps(stats)), 9)
    assert back.id == 9 and back.mean[3] == 1.8 and back.cov[3] == 1e-5 and back.valid
    assert mode_index("steady") == 3


def test_capture_activate_changes_decisions(pipeline, clock):
    # 20 s startup, then STEADY without baseline => NOT_EVALUATED (never NORMAL)
    feed(pipeline, clock, 30)
    d = last_decision(pipeline)
    assert d["mode"] == "STEADY" and d["outcome"] == "NOT_EVALUATED"
    pipeline.start_capture("node-a", "STEADY", 40)
    feed(pipeline, clock, 45, start_seq=31)
    rows = pipeline.db.writer.execute("SELECT id, status, n, mode FROM baselines").fetchall()
    assert len(rows) == 1 and rows[0]["status"] == "candidate" and rows[0]["n"] == 40
    d = last_decision(pipeline)
    assert d["outcome"] == "NOT_EVALUATED", "a candidate baseline must not influence decisions"
    pipeline.activate_baseline(rows[0]["id"])
    feed(pipeline, clock, 1, start_seq=76)
    d = last_decision(pipeline)
    assert d["outcome"] == "NORMAL" and d["baseline_id"] == rows[0]["id"]
    feed(pipeline, clock, 1, start_seq=77, vib_rms=0.9)
    assert last_decision(pipeline)["outcome"] == "CHECK_EQUIPMENT"
    types = [r[0] for r in pipeline.db.writer.execute("SELECT type FROM audit_events")]
    assert "baseline.captured" in types and "baseline.activated" in types


def test_capture_skips_untrusted_windows(pipeline, clock):
    feed(pipeline, clock, 25)
    pipeline.start_capture("node-a", "STEADY", 30)
    feed(pipeline, clock, 10, start_seq=26, samples_valid=100)    # incomplete => untrusted
    status = pipeline.capture_status()[0]
    assert status["accepted"] == 0 and status["skipped_untrusted"] == 10


def test_retire_clears_engine_baseline(pipeline, clock):
    feed(pipeline, clock, 25)
    pipeline.start_capture("node-a", "STEADY", 35, activate=True)
    feed(pipeline, clock, 36, start_seq=26)
    bid = pipeline.db.writer.execute("SELECT id FROM baselines WHERE status = 'active'"
                                     ).fetchone()[0]
    feed(pipeline, clock, 1, start_seq=62)
    assert last_decision(pipeline)["outcome"] == "NORMAL"
    pipeline.retire_baseline(bid)
    feed(pipeline, clock, 1, start_seq=63)
    assert last_decision(pipeline)["outcome"] == "NOT_EVALUATED"
