import math

import pytest

from algaops_gateway.audit.canonical import (
    CanonicalError,
    canonical_bytes,
    canonical_json,
    finite_or_none,
)


def test_sorted_compact_and_stable():
    a = canonical_json({"b": 1, "a": [1.5, "x", None, True], "c": {"z": 0, "y": -2}})
    assert a == '{"a":[1.5,"x",null,true],"b":1,"c":{"y":-2,"z":0}}'
    assert canonical_json({"c": {"y": -2, "z": 0}, "a": [1.5, "x", None, True], "b": 1}) == a


def test_utf8_not_escaped():
    assert canonical_bytes({"t": "28 °C"}) == '{"t":"28 °C"}'.encode("utf-8")


def test_float_repr_is_shortest_round_trip():
    assert canonical_json({"x": 0.1, "y": 1e-07, "z": 2.5e20}) == '{"x":0.1,"y":1e-07,"z":2.5e+20}'


@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_non_finite_rejected(bad):
    with pytest.raises(CanonicalError):
        canonical_json({"v": bad})


def test_non_string_keys_and_objects_rejected():
    with pytest.raises(CanonicalError):
        canonical_json({1: "x"})
    with pytest.raises(CanonicalError):
        canonical_json({"x": object()})


def test_finite_or_none():
    assert finite_or_none(math.nan) is None
    assert finite_or_none(None) is None
    assert finite_or_none(1) == 1.0
