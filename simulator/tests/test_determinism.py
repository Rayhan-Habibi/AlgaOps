"""Same scenario => byte-identical stream; fixtures on disk are up to date."""

import json

import pytest

from algaops_sim.replay import FIXTURE_DIR, generate_stream, read_fixture
from algaops_sim.scenario import list_scenarios, load_scenario

NAMES = [s.name for s in list_scenarios() if s.name not in ("normal_8h_replay",)]


@pytest.mark.parametrize("name", NAMES)
def test_stream_is_deterministic(name):
    sc = load_scenario(name)
    a, _ = generate_stream(sc)
    b, _ = generate_stream(sc)
    assert [(e.t_ms, e.topic, e.payload) for e in a] == [(e.t_ms, e.topic, e.payload) for e in b]


@pytest.mark.parametrize("name", [n for n in NAMES if n != "baseline_modes"])
def test_committed_fixture_matches_generator(name):
    sc = load_scenario(name)
    events, _ = generate_stream(sc)
    header, items = read_fixture(FIXTURE_DIR / f"{name}.jsonl")
    assert header["scenario"] == name and header["origin_ms"] == sc.start_ms
    assert len(items) == len(events)
    for it, e in zip(items, events):
        assert it["t_ms"] == e.t_ms - sc.start_ms and it["topic"] == e.topic
        assert it["payload"].encode() == e.payload


def test_live_mode_rebases_time_and_boot():
    sc = load_scenario("normal_steady")
    ev1, _ = generate_stream(sc, live=True, now_ms=2_000_000_000_000)
    ev2, _ = generate_stream(sc, live=True, now_ms=2_000_000_000_000)
    d1 = json.loads(next(e for e in ev1 if e.kind == "telemetry").payload)
    d2 = json.loads(next(e for e in ev2 if e.kind == "telemetry").payload)
    assert abs(d1["t_end_ms"] - 2_000_000_000_000) < 5000
    assert d1["boot_id"] != d2["boot_id"], "each live run is a fresh boot of the node"
