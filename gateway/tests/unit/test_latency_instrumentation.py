"""Unit tests for WP-4A-2/WP-4A-3 latency instrumentation and metric contracts."""

from __future__ import annotations

import json
import sqlite3
from types import SimpleNamespace

import pytest
from gwtest import TOPIC, make_doc

import algaops_gateway.analytics.evaluate as evaluator
from algaops_gateway.analytics.evaluate import QUICK, build_baselines, run_scenario
from algaops_gateway.analytics.metrics import Confusion
from algaops_gateway.pipeline import Pipeline, PipelineStats
from algaops_gateway.storage.database import Database


def test_pipeline_stats_segmented_latencies():
    stats = PipelineStats()
    # Record a sequence of known latency values
    # core: 1.0, 2.0, 3.0, 4.0, 5.0
    # commit: 2.0, 3.5, 4.5, 6.0, 8.0
    # sse: 2.2, 3.7, 4.8, 6.3, 8.5
    for i in range(1, 6):
        stats.note_latency(
            ms=float(i * 1.5),
            core_ms=float(i),
            commit_ms=float(i * 1.5),
            sse_ms=float(i * 1.5 + 0.5),
        )

    snap = stats.snapshot()

    # Verify all segmented percentiles are present
    assert "core_decision_ms" in snap
    assert "sqlite_commit_ms" in snap
    assert "sse_publish_ms" in snap
    assert "latency_ms" in snap

    core = snap["core_decision_ms"]
    assert core["samples"] == 5
    assert core["last"] == 5.0
    assert core["max"] == 5.0
    assert core["p50"] == 3.0

    commit = snap["sqlite_commit_ms"]
    assert commit["samples"] == 5
    assert commit["last"] == 7.5
    assert commit["max"] == 7.5

    sse = snap["sse_publish_ms"]
    assert sse["samples"] == 5
    assert sse["last"] == 8.0
    assert sse["max"] == 8.0


def test_pipeline_message_emits_latency_payload(pipeline, clock, monkeypatch):
    # Spy on event bus publish calls
    published = []
    real_publish = pipeline.events.publish

    def spy_publish(event, data):
        published.append((event, data))
        real_publish(event, data)

    monkeypatch.setattr(pipeline.events, "publish", spy_publish)

    doc = make_doc(1)
    t_recv = clock.t + 100
    ok = pipeline.handle_message(TOPIC, json.dumps(doc).encode("utf-8"), t_recv)
    assert ok is True

    decision_events = [data for ev_type, data in published if ev_type == "decision"]
    assert len(decision_events) == 1

    dec = decision_events[0]
    assert "latencies" in dec
    lat = dec["latencies"]

    # Verify required keys
    assert "core_decision_ms" in lat
    assert "sqlite_commit_ms" in lat
    assert "core_duration_ms" in lat
    assert "commit_duration_ms" in lat

    # Mathematical and monotonic invariants
    assert lat["core_decision_ms"] >= 0.0
    assert lat["sqlite_commit_ms"] >= 0.0
    assert lat["core_duration_ms"] >= 0.0
    assert lat["commit_duration_ms"] >= 0.0
    # In cumulative pipeline span, commit latency (T3 - T1) >= core decision latency (T2 - T1)
    assert lat["sqlite_commit_ms"] >= lat["core_decision_ms"]

    # Verify database persistence stores core_decision_ms in diagnostics.latency_ms
    row = pipeline.db.writer.execute(
        "SELECT latency_ms FROM diagnostics ORDER BY id DESC LIMIT 1"
    ).fetchone()
    assert row is not None
    assert row[0] == lat["core_decision_ms"]


def test_durable_sqlite_pragma(tmp_path):
    # Non-durable mode (default for fast evaluation): synchronous = OFF (0)
    db_fast = Database(tmp_path / "fast.db", durable=False)
    sync_fast = db_fast.writer.execute("PRAGMA synchronous").fetchone()[0]
    assert sync_fast == 0  # OFF
    db_fast.close()

    # Durable mode (benchmarking with fsync): synchronous = FULL (2)
    db_durable = Database(tmp_path / "durable.db", durable=True)
    sync_durable = db_durable.writer.execute("PRAGMA synchronous").fetchone()[0]
    assert sync_durable == 2  # FULL
    db_durable.close()


def test_metric_contract_edr_and_transport(lib, policy):
    baselines = build_baselines(lib, policy, "baseline_modes")

    # 1. Pure drop scenario: missing_packet (transport drop faults, no received fault windows)
    r_drop = run_scenario(lib, policy, "missing_packet", baselines, keep_timeline=False)
    k_drop = r_drop["kpi"]
    assert k_drop["edr"] is None
    assert k_drop["total_episodes"] == 0
    # 3 drop faults injected -> 3 gaps logged -> 100% drop gap detection recall
    tr_drop = k_drop["transport"]
    assert tr_drop["injected_drops"] == 3
    assert tr_drop["logged_gaps"] == 3
    assert tr_drop["drop_gap_recall"] == 1.0
    assert tr_drop["gaps_missing_samples"] == 6

    # 2. Reorder scenario: out_of_order_sequence
    r_reorder = run_scenario(lib, policy, "out_of_order_sequence", baselines, keep_timeline=False)
    k_reorder = r_reorder["kpi"]
    assert k_reorder["edr"] == 1.0
    tr_reorder = k_reorder["transport"]
    assert tr_reorder["injected_reorders"] == 1
    assert tr_reorder["reorder_detection_rate"] == 1.0
    assert tr_reorder["injected_drops"] == 0
    # Reorder induces 1 gap record in TrustGuard
    assert tr_reorder["logged_gaps"] == 1

    # 3. Delay scenario: stale_late_delivery
    r_delay = run_scenario(lib, policy, "stale_late_delivery", baselines, keep_timeline=False)
    k_delay = r_delay["kpi"]
    assert k_delay["edr"] == 1.0
    tr_delay = k_delay["transport"]
    assert tr_delay["injected_delays"] == 1
    assert tr_delay["delay_detection_rate"] == 1.0
    assert tr_delay["injected_drops"] == 0
    assert tr_delay["logged_gaps"] == 1

    # 4. Stuck sensor scenario: stuck_vibration
    # Verifies that raw window recall and FN exactly account for 8 warm-up windows
    r_stuck = run_scenario(lib, policy, "stuck_vibration", baselines, keep_timeline=False)
    k_stuck = r_stuck["kpi"]
    assert k_stuck["edr"] == 1.0  # Episode as a whole is detected
    assert k_stuck["fn_windows"] == 8  # Exactly 8 warm-up windows
    trust = r_stuck["trust_detection"]
    assert trust["tp"] == 62
    assert trust["fn"] == 8
    assert trust["tp"] + trust["fn"] == 70
    assert abs(k_stuck["raw_window_recall"] - (62 / 70)) < 1e-4


def _kpi_of_stubbed_run(lib, policy, monkeypatch, name, **fields):
    run = run_scenario(lib, policy, "normal_steady", None, keep_timeline=False)
    monkeypatch.setattr(evaluator, "run_scenario", lambda *a, **k: run | {"name": name, **fields})
    return evaluator.evaluate(lib, policy, (name,))["kpi"]


def test_kpi_8h_false_positives_count_windows_and_alerts(lib, policy, monkeypatch):
    kpi = _kpi_of_stubbed_run(lib, policy, monkeypatch, "normal_8h_replay",
                              alerts_opened={"CHECK_EQUIPMENT": 1},
                              outcome_counts={"NORMAL": 100, "CHECK_EQUIPMENT": 40,
                                              "CHECK_SENSOR": 2, "NOT_EVALUATED": 3})
    assert kpi["fp_window_8h"] == 42
    assert kpi["fp_alert_8h"] == 1


def test_kpi_8h_false_positives_are_unknown_without_the_replay(lib, policy, monkeypatch):
    kpi = _kpi_of_stubbed_run(lib, policy, monkeypatch, "normal_steady")
    assert kpi["fp_window_8h"] is None
    assert kpi["fp_alert_8h"] is None


def test_edr_counts_only_detections_within_the_latency_limit(lib, policy, monkeypatch):
    baselines = build_baselines(lib, policy, "baseline_modes")
    k = run_scenario(lib, policy, "stuck_vibration", baselines, keep_timeline=False)["kpi"]
    assert (k["detected_episodes"], k["total_episodes"], k["edr"]) == (2, 2, 1.0)

    # the repeated-features episode needs 8 messages
    monkeypatch.setitem(evaluator.EDR_MAX_LATENCY_MESSAGES, "stuck_vibration", 7)
    k = run_scenario(lib, policy, "stuck_vibration", baselines, keep_timeline=False)["kpi"]
    assert (k["detected_episodes"], k["total_episodes"], k["edr"]) == (1, 2, 0.5)


def test_edr_denominator_excludes_equipment_episodes(lib, policy):
    baselines = build_baselines(lib, policy, "baseline_modes")
    k = run_scenario(lib, policy, "equipment_vibration_anomaly", baselines,
                     keep_timeline=False)["kpi"]
    assert k["edr"] is None
    assert k["total_episodes"] == 0
    assert (k["equipment_detected"], k["equipment_episodes"]) == (1, 1)


def test_reorder_and_delay_detection_need_the_trustguard_flag():
    results = [{"window": 100, "doc": {"trust": {"flags": ["MISSING_PACKET"]}}},
               {"window": 101, "doc": None}]
    assert not evaluator._flagged(results, "OUT_OF_ORDER_SEQ", 100, 101)
    results[0]["doc"]["trust"]["flags"].append("OUT_OF_ORDER_SEQ")
    assert evaluator._flagged(results, "OUT_OF_ORDER_SEQ", 100, 101)
    assert not evaluator._flagged(results, "OUT_OF_ORDER_SEQ", 101, 102)


def test_a_drop_is_matched_to_the_gap_that_covers_it():
    drop = SimpleNamespace(start=10, params={"count": 2})
    seq_of = {10: (7, 11), 11: (7, 12)}
    assert evaluator._drop_logged(drop, seq_of, [{"boot_id": 7, "from_seq": 11, "to_seq": 12}])
    assert not evaluator._drop_logged(drop, seq_of, [{"boot_id": 7, "from_seq": 50, "to_seq": 51}])
    assert not evaluator._drop_logged(drop, seq_of, [{"boot_id": 8, "from_seq": 11, "to_seq": 12}])
