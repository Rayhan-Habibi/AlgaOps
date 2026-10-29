import json

from gwtest import make_doc

from algaops_gateway.audit.canonical import canonical_json
from algaops_gateway.core.engine import NodeEngine
from algaops_gateway.diagnostics.decision import DecisionRecord
from algaops_gateway.ingest.payload_schema import parse_telemetry


def decide(lib, policy, docs):
    eng = NodeEngine(lib, policy, "node-a")
    out = None
    for d in docs:
        nw = parse_telemetry(json.dumps(d).encode(), d["t_end_ms"] + 50, "node-a")
        out = eng.process(nw.to_c())
    eng.close()
    return DecisionRecord.from_core(lib, out, node_id="node-a", data_source="simulator",
                                    decided_ms=123, policy_hash=policy.hash, window_digest="ab")


def test_record_fields_copied_from_core(lib, policy):
    rec = decide(lib, policy, [make_doc(1)])
    d = rec.doc
    assert d["schema"] == "algaops.diagnostic_event.v1" and d["kind"] == "window"
    assert d["node_id"] == "node-a" and d["seq"] == 1 and d["boot_id"] == 7
    assert d["trust"]["trusted"] is True and d["trust"]["flags"] == []
    assert d["mode"] in ("STARTUP", "STEADY") and d["policy_hash"] == policy.hash
    assert d["engine_version"] == lib.version and d["window_digest"] == "ab"
    assert set(d["methods"]) == {"fixed", "zscore", "mahalanobis"}
    assert canonical_json(d) == rec.canonical()


def test_untrusted_record(lib, policy):
    rec = decide(lib, policy, [make_doc(1), make_doc(2, samples_valid=100)])
    d = rec.doc
    assert d["outcome"] == "CHECK_SENSOR" and not d["trust"]["trusted"]
    assert "INCOMPLETE_WINDOW" in d["trust"]["flags"] and d["trust"]["blocking"]
    assert all(m["status"] == "skipped" for m in d["methods"].values())
    assert d["mode"] == "UNKNOWN" and d["decided_by"] is None
