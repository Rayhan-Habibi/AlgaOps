"""Offline, labelled evaluation of the complete gateway path on synthetic scenarios.

Each scenario's received-message stream (from the simulator) is pushed
through a real Pipeline (payload validation, idempotency, C core, SQLite,
audit chain) with a simulated clock and 1 s ticks.  Ground-truth labels are
kept on the side and are only compared AFTER each decision has been stored —
they never enter the pipeline, so they cannot leak into decisions.

Results describe behaviour on designed SYNTHETIC scenarios.  They are not an
estimate of field performance on a real pump or PBR.
"""

from __future__ import annotations

import json
import tempfile
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any

from ..core.engine import NodeEngine, Policy
from ..core.libalgaops import CoreLibrary
from ..diagnostics.baseline_manager import BaselineCapture, baseline_to_stats
from ..ingest.payload_schema import PayloadError, parse_telemetry
from ..pipeline import Pipeline
from ..storage.database import Database
from .metrics import Confusion, latency_stats, rnd

DISCLAIMER = ("SYNTHETIC evaluation: deterministic simulator scenarios designed by the project "
              "team. The numbers show how the implemented logic behaves on these scenarios; "
              "they are NOT measured on a real pump, blower or PBR and do not establish "
              "industrial diagnostic performance.")

METHODS = ("fixed", "zscore", "mahalanobis")
QUICK = ("normal_steady", "mode_transitions", "stuck_vibration", "missing_packet",
         "duplicate_sequence", "out_of_order_sequence", "bad_timestamp", "stale_late_delivery",
         "out_of_range_spike", "non_finite_values", "malformed_payload", "self_test_failure",
         "sensor_disagreement", "equipment_vibration_anomaly", "equipment_thermal_anomaly",
         "suspicious_plus_blocking", "disconnect_reconnect_replay")
FULL = QUICK + ("stuck_temperature", "normal_8h_replay")

BLOCKING_TRUTHS = ("sensor_fault", "transport_fault")
# Latest detection (messages after onset) that still counts towards EDR (DEC-001): detectors
# that need a history get their window length, every other blocking fault is immediate.
EDR_MAX_LATENCY_MESSAGES = {"stuck_vibration": 10, "stuck_temperature": 1000}


def _sim():
    try:
        from algaops_sim import replay, scenario
    except ImportError as exc:  # pragma: no cover
        raise RuntimeError("the simulator package (simulator/) is required for evaluation") from exc
    return replay, scenario


class SimClock:
    def __init__(self, t: int) -> None:
        self.t = t

    def __call__(self) -> int:
        return self.t


def build_baselines(lib: CoreLibrary, policy: Policy, scenario_name: str) -> list:
    """Capture OFF/STARTUP/STEADY baselines from a commissioning scenario using
    the same trusted-window rule as live capture."""
    replay, scen = _sim()
    sc = scen.load_scenario(scenario_name)
    events, _ = replay.generate_stream(sc)
    eng = NodeEngine(lib, policy, sc.node_id)
    fmask = sum(1 << i for i, n in enumerate(("vib_rms_g", "vib_p2p_g", "temp_c", "current_a"))
                if policy.effective.get(f"health.features.{n}", 1))
    caps = {}
    for mode in ("OFF", "STARTUP", "STEADY"):
        c = BaselineCapture(lib, fmask)
        c.start(sc.node_id, mode, 10**9, 0)
        caps[mode] = c
    for e in events:
        if e.kind != "telemetry":
            continue
        try:
            nw = parse_telemetry(e.payload, e.t_ms, sc.node_id)
        except PayloadError:
            continue
        cw = nw.to_c()
        d = eng.process(cw)
        for c in caps.values():
            c.offer(sc.node_id, cw, d)
    eng.close()
    out = []
    min_n = int(policy.effective.get("baseline.min_samples", 10))
    for mode, c in caps.items():
        s = c.sessions.get(sc.node_id)
        if s is None or s.accepted < min_n:
            continue
        out.append(c.finish(s, min_n))
    return out


def run_scenario(lib: CoreLibrary, policy: Policy, name: str, baselines: list | None,
                 keep_timeline: bool = True, durable: bool = False) -> dict[str, Any]:
    replay, scen = _sim()
    sc = scen.load_scenario(name)
    events, windows = replay.generate_stream(sc)
    with tempfile.TemporaryDirectory(prefix="algaops_eval_") as tmp:
        db = Database(Path(tmp) / "eval.db", durable=durable)
        clock = SimClock(sc.start_ms)
        pipe = Pipeline(lib=lib, policy=policy, db=db, gateway_id="evaluation", clock=clock,
                        checkpoint_every_events=10**9, checkpoint_every_s=10**9)
        pipe.open()
        if baselines and sc.baseline not in (None, "none"):
            for bl in baselines:
                bid = pipe._store_baseline(sc.node_id, baseline_to_stats(bl),
                                           {"kind": "evaluation", "scenario": sc.baseline},
                                           "baseline.imported")
                pipe._activate(bid)
        results: list[dict[str, Any]] = []
        timeouts: list[dict[str, Any]] = []
        proc_ms: list[float] = []
        next_tick = sc.start_ms
        conn = db.writer

        def do_ticks(until: int) -> None:
            nonlocal next_tick
            while next_tick + 1000 <= until:
                next_tick += 1000
                clock.t = next_tick
                if pipe.tick(next_tick):
                    r = conn.execute("SELECT outcome FROM diagnostics ORDER BY id DESC LIMIT 1"
                                     ).fetchone()
                    timeouts.append({"t_ms": next_tick - sc.start_ms, "outcome": r[0]})

        for e in sorted(events, key=lambda x: x.t_ms):
            do_ticks(e.t_ms)
            clock.t = e.t_ms
            before = (pipe.stats.accepted, pipe.stats.duplicate_deliveries, pipe.stats.rejected)
            t0 = time.perf_counter()
            pipe.handle_message(e.topic, e.payload, e.t_ms)
            proc_ms.append((time.perf_counter() - t0) * 1000.0)
            if e.kind != "telemetry":
                continue
            after = (pipe.stats.accepted, pipe.stats.duplicate_deliveries, pipe.stats.rejected)
            doc = None
            if after[1] > before[1]:
                outcome = "DROPPED"
            else:
                r = conn.execute("SELECT decision_json FROM diagnostics WHERE kind != "
                                 "'stream_timeout' ORDER BY id DESC LIMIT 1").fetchone()
                doc = json.loads(r[0]) if r else None
                outcome = doc["outcome"] if doc else "NONE"
            lab = e.label
            results.append({"t_ms": e.t_ms - sc.start_ms, "window": e.window_index,
                            "truth": lab.truth, "fault": lab.fault,
                            "expected": list(lab.expected), "true_mode": lab.mode,
                            "outcome": outcome, "doc": doc})
        do_ticks((events[-1].t_ms if events else sc.start_ms) + 2000)
        alerts = Counter(r[0] for r in conn.execute("SELECT kind FROM alerts"))
        chain_events = conn.execute("SELECT COUNT(*) FROM audit_events").fetchone()[0]
        gaps = [dict(r) for r in conn.execute("SELECT boot_id, from_seq, to_seq, missing FROM gaps")]
        pipe.close()
        db.close()
    return summarize(sc, windows, results, timeouts, alerts, chain_events, proc_ms,
                     keep_timeline, gaps=gaps, policy=policy, durable=durable)


def _rate(part: int, whole: int) -> float | None:
    return rnd(part / whole, 4) if whole else None


def _drop_logged(drop, seq_of: dict[int, tuple[int, int]], gaps: list[dict]) -> bool:
    """One gap row covers every sequence number the drop removed.  Matched rather than
    counted: gaps opened by reordering or delay must not stand in for a drop."""
    lost = [seq_of[i] for i in range(drop.start, drop.start + int(drop.params.get("count", 1)))
            if i in seq_of]
    return bool(lost) and any(g["boot_id"] == lost[0][0] and g["from_seq"] <= lost[0][1]
                              and lost[-1][1] <= g["to_seq"] for g in gaps)


def _flagged(results: list[dict], flag: str, first: int, last: int) -> bool:
    """A window of [first, last] carries the TrustGuard flag set by the core."""
    return any(r["doc"] and first <= r["window"] <= last and flag in r["doc"]["trust"]["flags"]
               for r in results)


def _false_alarm_windows(run: dict[str, Any]) -> dict[str, int]:
    return {k: v for k, v in run["outcome_counts"].items()
            if k in ("CHECK_SENSOR", "CHECK_EQUIPMENT")}


def summarize(sc, windows, results, timeouts, alerts, chain_events, proc_ms,
              keep_timeline: bool, gaps: list[dict] | None = None,
              policy: Policy | None = None, durable: bool = False) -> dict[str, Any]:
    gaps = gaps or []
    matrix: dict[str, Counter] = defaultdict(Counter)
    trust = Confusion()
    equip = Confusion()
    b0 = Confusion()
    methods = {m: Confusion() for m in METHODS}
    method_windows = Counter()
    correct = 0
    attribution_errors = 0
    untrusted_equipment = 0
    mode_hits = Counter()

    fixed_vib_limit = 1.00
    fixed_temp_limit = 45.0
    if policy is not None and hasattr(policy, "effective"):
        try:
            fixed_vib_limit = float(policy.effective.get("health.fixed.steady.vib_max_g", 1.00))
            fixed_temp_limit = float(policy.effective.get("health.fixed.steady.temp_max_c", 45.0))
        except (ValueError, TypeError):
            pass

    for r in results:
        out, truth = r["outcome"], r["truth"]
        matrix[truth][out] += 1
        if out in r["expected"]:
            correct += 1
        doc = r["doc"]
        sensorish = truth in BLOCKING_TRUTHS
        if "DROPPED" not in r["expected"]:
            trust.add(out == "CHECK_SENSOR", sensorish)
        if out == "CHECK_EQUIPMENT" and sensorish:
            attribution_errors += 1
        if out == "CHECK_EQUIPMENT" and doc and not doc["trust"]["trusted"]:
            untrusted_equipment += 1
        if not sensorish and doc:
            equip.add(out == "CHECK_EQUIPMENT", truth == "equipment_fault")
            if doc["trust"]["trusted"] and doc["mode"] == "STEADY":
                for m in METHODS:
                    res = doc["methods"].get(m, {})
                    if res.get("status") == "ok":
                        methods[m].add(bool(res.get("anomalous")), truth == "equipment_fault")
                        method_windows[m] += 1
        if doc and doc["trust"]["trusted"] and doc["mode"] != "UNKNOWN":
            mode_hits["evaluated"] += 1
            mode_hits["agree"] += int(doc["mode"] == r["true_mode"])

        # Baseline B0: Naive ungated single-threshold alarm (no TrustGate, no mode classification)
        if doc:
            f = doc.get("features") or {}
            v = f.get("vib_rms_g")
            t = f.get("temp_c")
            b0_anom = bool((v is not None and v > fixed_vib_limit) or
                           (t is not None and t > fixed_temp_limit))
            b0.add(b0_anom, truth == "equipment_fault")
        elif "DROPPED" not in r["expected"]:
            b0.add(False, truth == "equipment_fault")

    # event-level detection latency per injected fault
    events = []
    by_fault: dict[tuple[str, int], list[dict]] = {}
    seg = 0
    prev = None
    for r in results:
        key = r["fault"] if r["truth"] != "normal" and "DROPPED" not in r["expected"] else None
        if key != prev:
            seg += 1
            prev = key
        if key:
            by_fault.setdefault((key, seg), []).append(r)
    for (fault, _), rs in by_fault.items():
        want = rs[0]["expected"]
        hit = next((x for x in rs if x["outcome"] in want), None)
        events.append({
            "fault": fault, "truth": rs[0]["truth"], "messages": len(rs),
            "onset_t_s": rnd(rs[0]["t_ms"] / 1000.0, 2), "detected": hit is not None,
            "latency_messages": (rs.index(hit) if hit else None),
            "latency_s": rnd((hit["t_ms"] - rs[0]["t_ms"]) / 1000.0, 2) if hit else None,
            "within_latency_limit": (hit is not None and
                                     rs.index(hit) <= EDR_MAX_LATENCY_MESSAGES.get(fault, 0)),
            "detected_fraction": rnd(sum(x["outcome"] in want for x in rs) / len(rs))})

    blocking = [e for e in events if e["truth"] in BLOCKING_TRUTHS]
    equipment = [e for e in events if e["truth"] == "equipment_fault"]
    detected_blocking = sum(e["within_latency_limit"] for e in blocking)

    drops = [f for f in sc.network if f.type == "drop"]
    reorders = [f for f in sc.network if f.type == "reorder"]
    delays = [f for f in sc.network if f.type == "delay"]
    seq_of = {w.index: (w.boot_id, w.seq) for w in windows}
    drops_logged = sum(_drop_logged(f, seq_of, gaps) for f in drops)
    reorders_detected = sum(_flagged(results, "OUT_OF_ORDER_SEQ", f.start, f.start + 1)
                            for f in reorders)
    delays_detected = sum(_flagged(results, "STALE", f.start, (f.end or f.start + 1) - 1)
                          for f in delays)
    drop_gap_recall = _rate(drops_logged, len(drops))

    out = {
        "name": sc.name, "description": sc.description.strip(),
        "windows_generated": len(windows), "messages": len(results),
        "durable": durable,
        "outcome_counts": dict(Counter(r["outcome"] for r in results)),
        "truth_vs_outcome": {k: dict(v) for k, v in matrix.items()},
        "expected_match_rate": rnd(correct / len(results)) if results else None,
        "trust_detection": trust.as_dict(),
        "equipment_detection": equip.as_dict(),
        "methods_steady": {m: methods[m].as_dict() | {"windows": method_windows[m]}
                           for m in METHODS},
        "baseline_b0": b0.as_dict(),
        "attribution_errors": attribution_errors,
        "untrusted_check_equipment": untrusted_equipment,
        "mode_agreement": rnd(mode_hits["agree"] / mode_hits["evaluated"])
        if mode_hits["evaluated"] else None,
        "fault_events": events,
        "kpi": {
            "edr": _rate(detected_blocking, len(blocking)),
            "detected_episodes": detected_blocking,
            "total_episodes": len(blocking),
            "equipment_episodes": len(equipment),
            "equipment_detected": sum(e["detected"] for e in equipment),
            "raw_window_recall": trust.recall,
            "fn_windows": trust.fn,
            "transport": {
                "injected_drops": len(drops),
                "drops_logged": drops_logged,
                "dropped_packets": sum(int(f.params.get("count", 1)) for f in drops),
                "logged_gaps": len(gaps),
                "gaps_missing_samples": sum(g["missing"] for g in gaps),
                "drop_gap_recall": drop_gap_recall,
                "injected_reorders": len(reorders),
                "reorders_detected": reorders_detected,
                "reorder_detection_rate": _rate(reorders_detected, len(reorders)),
                "injected_delays": len(delays),
                "delays_detected": delays_detected,
                "delay_detection_rate": _rate(delays_detected, len(delays)),
            },
            "glr": drop_gap_recall,
        },
        "stream_timeouts": len(timeouts),
        "alerts_opened": dict(alerts),
        "audit_events": chain_events,
        "processing_ms_per_message": latency_stats(proc_ms),
        "duration_s": rnd(len(windows) * sc.window_ms / 1000.0, 1),
    }
    if keep_timeline:
        out["timeline"] = [{k: v for k, v in r.items() if k != "doc"} |
                           ({"flags": r["doc"]["trust"]["flags"], "mode": r["doc"]["mode"],
                             "reason": r["doc"]["reason"][:200]} if r["doc"] else {})
                           for r in results]
        out["timeouts"] = timeouts
    return out


def evaluate(lib: CoreLibrary, policy: Policy, scenarios: tuple[str, ...] = QUICK,
             baseline_scenario: str = "baseline_modes", keep_timeline: bool = False,
             durable: bool = False) -> dict[str, Any]:
    t0 = time.perf_counter()
    baselines = build_baselines(lib, policy, baseline_scenario)
    runs = [run_scenario(lib, policy, n, baselines, keep_timeline, durable=durable) for n in scenarios]
    overall_trust, overall_equip, overall_b0 = Confusion(), Confusion(), Confusion()
    overall_methods = {m: Confusion() for m in METHODS}
    tally: Counter = Counter()
    for r in runs:
        for src, dst in ((r["trust_detection"], overall_trust),
                         (r["equipment_detection"], overall_equip),
                         (r["baseline_b0"], overall_b0)):
            dst.merge(Confusion(src["tp"], src["fp"], src["tn"], src["fn"]))
        for m in METHODS:
            s = r["methods_steady"][m]
            overall_methods[m].merge(Confusion(s["tp"], s["fp"], s["tn"], s["fn"]))
        k = r["kpi"]
        tally.update({key: k[key] for key in ("total_episodes", "detected_episodes",
                                              "equipment_episodes", "equipment_detected")})
        tally.update({key: k["transport"][key] for key in (
            "injected_drops", "drops_logged", "logged_gaps", "injected_reorders",
            "reorders_detected", "injected_delays", "delays_detected")})

    glr = _rate(tally["drops_logged"], tally["injected_drops"])
    normal8 = next((r for r in runs if r["name"] == "normal_8h_replay"), None)

    kpi_dict = {
        "edr": _rate(tally["detected_episodes"], tally["total_episodes"]),
        "detected_episodes": tally["detected_episodes"],
        "total_episodes": tally["total_episodes"],
        "edr_max_latency_messages": EDR_MAX_LATENCY_MESSAGES,
        "equipment_episodes": tally["equipment_episodes"],
        "equipment_detected": tally["equipment_detected"],
        "raw_window_recall": overall_trust.recall,
        "fn_windows": overall_trust.fn,
        # None, not 0: without the replay these were not measured
        "fp_alert_8h": sum(normal8["alerts_opened"].values()) if normal8 else None,
        "fp_window_8h": sum(_false_alarm_windows(normal8).values()) if normal8 else None,
        "transport": {
            "drop_gap_detection_recall": glr,
            "reorder_detection_rate": _rate(tally["reorders_detected"], tally["injected_reorders"]),
            "delay_detection_rate": _rate(tally["delays_detected"], tally["injected_delays"]),
            "total_gap_records": tally["logged_gaps"],
            "injected_drops": tally["injected_drops"],
            "gap_record_to_drop_ratio": _rate(tally["logged_gaps"], tally["injected_drops"]),
        },
        "glr": glr,
    }
    baselines_comp = {
        "b0_naive_ungated": overall_b0.as_dict(),
        "b1_gated_fixed": overall_methods["fixed"].as_dict(),
        "zscore_default": overall_methods["zscore"].as_dict(),
        "mahalanobis": overall_methods["mahalanobis"].as_dict(),
    }

    return {
        "schema": "algaops.evaluation.v1",
        "generated_ms": time.time_ns() // 1_000_000,
        "engine_version": lib.version,
        "policy_hash": policy.hash,
        "policy_name": policy.meta.get("name"),
        "disclaimer": DISCLAIMER,
        "durable": durable,
        "sqlite_synchronous": "FULL" if durable else "OFF",
        "evidence_label": "SYNTHETIC SCENARIOS (HOST EVALUATOR ONLY — NOT PHYSICAL HARDWARE)",
        "baseline_scenario": baseline_scenario,
        "baselines": [baseline_to_stats(b) for b in baselines],
        "scenarios": runs,
        "kpi": kpi_dict,
        "baselines_comparison": baselines_comp,
        "overall": {
            "trust_detection": overall_trust.as_dict(),
            "equipment_detection": overall_equip.as_dict(),
            "methods_steady": {m: c.as_dict() for m, c in overall_methods.items()},
            "baselines_comparison": baselines_comp,
            "kpi": kpi_dict,
            "attribution_errors": sum(r["attribution_errors"] for r in runs),
            "untrusted_check_equipment": sum(r["untrusted_check_equipment"] for r in runs),
            "alerts_in_8h_normal": normal8["alerts_opened"] if normal8 else None,
            "window_false_alarms_in_8h_normal": _false_alarm_windows(normal8) if normal8 else None,
        },
        "wall_time_s": rnd(time.perf_counter() - t0, 2),
    }
