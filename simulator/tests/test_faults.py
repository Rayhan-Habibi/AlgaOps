"""Fault injection produces the intended observable effects and labels."""

import json

from algaops_sim.replay import generate_stream
from algaops_sim.scenario import load_scenario


def stream(name):
    sc = load_scenario(name)
    events, windows = generate_stream(sc)
    return sc, [e for e in events if e.kind == "telemetry"], windows


def docs(events):
    out = []
    for e in events:
        try:
            out.append((e, json.loads(e.payload)))
        except ValueError:
            out.append((e, None))
    return out


def test_frozen_vibration_has_zero_p2p():
    sc, tele, _ = stream("stuck_vibration")
    frozen = [d for e, d in docs(tele) if e.label.fault == "stuck_vibration" and
              150 <= e.window_index < 180]
    assert len(frozen) == 30 and all(d["vib"]["p2p_g"] == [0.0, 0.0, 0.0] for d in frozen)
    repeat = [d["vib"] for e, d in docs(tele) if 200 <= e.window_index < 240]
    assert all(v == repeat[0] for v in repeat), "repeat mode sends identical features"


def test_missing_packets_leave_sequence_gaps():
    sc, tele, _ = stream("missing_packet")
    seqs = [json.loads(e.payload)["seq"] for e in tele]
    assert len(seqs) == sc.duration_windows - 6
    assert 61 not in seqs and {101, 102, 103} & set(seqs) == set()


def test_duplicates_exact_and_conflicting():
    sc, tele, _ = stream("duplicate_sequence")
    exact = [e for e in tele if e.redelivery]
    assert len(exact) == 1 and exact[0].label.expected == ("DROPPED",)
    seq141 = [e for e in tele if json.loads(e.payload)["seq"] == 141 and not e.redelivery]
    assert len(seq141) == 2 and seq141[0].payload != seq141[1].payload


def test_reorder_swaps_delivery():
    sc, tele, _ = stream("out_of_order_sequence")
    seqs = [json.loads(e.payload)["seq"] for e in tele]
    i = seqs.index(102)
    assert seqs[i + 1] == 101
    assert tele[i + 1].label.fault == "out_of_order"


def test_disconnect_buffers_and_flushes_in_order():
    sc, tele, _ = stream("disconnect_reconnect_replay")
    seqs = [json.loads(e.payload)["seq"] for e in tele]
    assert seqs[:170] == list(range(1, 171)), "no window lost (buffer large enough)"
    late = [e for e in tele if e.label.fault == "late_delivery"]
    assert len(late) == 22 and all(e.t_ms - json.loads(e.payload)["t_end_ms"] > 10_000
                                   for e in late)
    after = [json.loads(e.payload) for e in tele if e.window_index is not None and
             e.window_index >= 170]
    assert after[0]["seq"] == 1 and after[0]["boot_id"] != json.loads(tele[0].payload)["boot_id"]


def test_equipment_anomaly_labels_only_when_running():
    sc, tele, windows = stream("equipment_vibration_anomaly")
    eq = [e for e in tele if e.label.truth == "equipment_fault"]
    assert len(eq) == 90
    rms = [json.loads(e.payload)["vib"]["rms_g"] for e in eq]
    assert rms[-1] > 2.5 * rms[0] / 1.1


def test_sensor_disagreement_keeps_current():
    sc, tele, _ = stream("sensor_disagreement")
    d = [json.loads(e.payload) for e in tele if e.label.fault == "sensor_disagreement"]
    assert all(x["vib"]["rms_g"] < 0.02 and x["current_a"] > 1.5 for x in d)
