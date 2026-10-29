from algaops_gateway.analytics.metrics import Confusion, latency_stats


def test_confusion_rates():
    c = Confusion()
    for pred, act in [(True, True)] * 8 + [(False, True)] * 2 + [(True, False)] + \
            [(False, False)] * 89:
        c.add(pred, act)
    assert (c.tp, c.fn, c.fp, c.tn) == (8, 2, 1, 89)
    assert abs(c.precision - 8 / 9) < 1e-12
    assert c.recall == 0.8 and abs(c.fpr - 1 / 90) < 1e-12
    d = c.as_dict()
    assert d["support"] == 10 and d["negatives"] == 90 and d["f1"] is not None


def test_undefined_rates_are_none():
    c = Confusion()
    assert c.precision is None and c.recall is None and c.fpr is None and c.f1 is None


def test_latency_stats():
    s = latency_stats([5.0, 1.0, 3.0, 2.0, 4.0])
    assert s["n"] == 5 and s["p50"] == 3.0 and s["max"] == 5.0 and s["mean"] == 3.0
    assert latency_stats([])["n"] == 0
