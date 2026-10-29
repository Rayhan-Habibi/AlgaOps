import json
import math

import pytest
from gwtest import make_doc

from algaops_gateway.core.libalgaops import (
    HAS_AXES,
    HAS_CURRENT,
    HAS_P2P,
    HAS_RECEIVED,
    HAS_SAMPLE_COUNTS,
    HAS_SELF_TEST,
    HAS_TIME_SYNC,
    HAS_TIMING,
)
from algaops_gateway.ingest.payload_schema import (
    NormalizedWindow,
    PayloadError,
    parse_telemetry,
)


def enc(d):
    return json.dumps(d).encode()


def test_valid_payload_and_presence_bits():
    w = parse_telemetry(enc(make_doc(5)), 123, "node-a")
    assert (w.node_id, w.seq, w.boot_id, w.received_ms) == ("node-a", 5, 7, 123)
    c = w.to_c()
    for bit in (HAS_RECEIVED, HAS_TIMING, HAS_SAMPLE_COUNTS, HAS_AXES, HAS_P2P, HAS_CURRENT,
                HAS_SELF_TEST, HAS_TIME_SYNC):
        assert c.presence & bit
    assert c.received_ms == 123 and c.sequence_id == 5


def test_record_roundtrip():
    w = parse_telemetry(enc(make_doc(3, temp_c=None)), 9, "node-a")
    rec = w.to_record()
    assert rec["temp_c"] is None and rec["current_present"] is True
    back = NormalizedWindow.from_record(json.loads(json.dumps(rec)))
    assert math.isnan(back.temp_c) and back.seq == 3
    assert back.current_a == make_doc(3)["current_a"]
    assert back.to_c().presence == w.to_c().presence


@pytest.mark.parametrize("mutate,reason", [
    (lambda d: d.update(schema="algaops.telemetry_window.v0"), "unsupported schema"),
    (lambda d: d.update(label="equipment_fault"), "unexpected field"),
    (lambda d: d.pop("temp_c"), "missing required"),
    (lambda d: d.update(seq="1"), "seq"),
    (lambda d: d.update(seq=True), "seq"),
    (lambda d: d.update(seq=-1), "seq"),
    (lambda d: d.update(boot_id=2**32), "boot_id"),
    (lambda d: d.update(source="factory"), "source"),
    (lambda d: d.update(node_id="bad/node"), "node_id"),
    (lambda d: d.update(node_id="node-b"), "does not match topic"),
    (lambda d: d["vib"].update(rms_axis_g=[1, 2]), "rms_axis_g"),
    (lambda d: d["vib"].update(extra=1), "unexpected vib"),
    (lambda d: d.update(faults=["MELTDOWN"]), "faults"),
    (lambda d: d.update(self_test="maybe"), "self_test"),
    (lambda d: d.update(time_synced=1), "time_synced"),
    (lambda d: d.pop("window_ms"), "together"),
    (lambda d: d.update(health={"cpu": 1}), "health"),
])
def test_structural_errors(mutate, reason):
    d = make_doc(1)
    mutate(d)
    with pytest.raises(PayloadError, match=reason):
        parse_telemetry(enc(d), 1, "node-a")


def test_not_json_not_object_too_large():
    for raw, reason in [(b"\xff\xfe", "UTF-8"), (b"{not json", "JSON"), (b"[1,2]", "object"),
                        (b"x" * 9000, "exceeds")]:
        with pytest.raises(PayloadError, match=reason):
            parse_telemetry(raw, 1, "node-a")


def test_nan_tokens_are_measurements_not_errors():
    raw = enc(make_doc(1)).replace(b'"temp_c": 31.0', b'"temp_c": NaN')
    w = parse_telemetry(raw, 1, "node-a")
    assert math.isnan(w.temp_c)
    w = parse_telemetry(enc(make_doc(1, current_a=None)), 1, "node-a")
    assert w.current_present and math.isnan(w.current_a)


def test_node_hint_kept_for_attribution():
    with pytest.raises(PayloadError) as ei:
        parse_telemetry(b"garbage", 1, "node-z")
    assert ei.value.node_hint == "node-z"
