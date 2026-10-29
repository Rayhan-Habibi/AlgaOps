from algaops_gateway.ingest.freshness import LATE, LIVE, NEVER, STALE, classify_liveness, completeness


def test_liveness_classes():
    assert classify_liveness(10_000, None, 1280, 15_000) == NEVER
    assert classify_liveness(10_000, 9_000, 1280, 15_000) == LIVE
    assert classify_liveness(20_000, 10_000, 1280, 15_000) == LATE
    assert classify_liveness(40_000, 10_000, 1280, 15_000) == STALE
    assert classify_liveness(5_000, 10_000, 1280, 15_000) == LIVE   # clock skew: never negative


def test_completeness():
    assert completeness(0, 0) is None
    assert completeness(8, 10) == 0.8
    assert completeness(12, 10) == 1.0
