"""Audit chain append/commit/rollback and end-to-end verification of a run."""

import json

from gwtest import feed

from algaops_gateway.audit.ascon import Hasher
from algaops_gateway.audit.hash_chain import AuditChain, iter_events
from algaops_gateway.audit.verify import verify_database


def test_append_commit_rollback(lib, db):
    chain = AuditChain(Hasher(lib), "gw")
    chain.load_head(db.writer)
    assert chain.head == (0, "0" * 64)
    with db.transaction() as tx:
        e1 = chain.append(tx, "gateway.started", 1, None, {"x": 1})
        e2 = chain.append(tx, "audit.verified", 2, None, {"ok": True})
    chain.commit()
    assert chain.head == (2, e2.hash) and e2.prev_hash == e1.hash
    try:
        with db.transaction() as tx:
            chain.append(tx, "audit.verified", 3, None, {"ok": False})
            raise RuntimeError("simulated crash before commit")
    except RuntimeError:
        chain.rollback()
    assert chain.head == (2, e2.hash)
    assert [e.seq for e in iter_events(db.writer)] == [1, 2]
    body = json.loads(e1.payload)
    assert body["type"] == "gateway.started" and body["seq"] == 1 and body["gateway_id"] == "gw"


def test_unknown_event_type_rejected(lib, db):
    chain = AuditChain(Hasher(lib), "gw")
    chain.load_head(db.writer)
    with db.transaction() as tx:
        try:
            chain.append(tx, "pump.start", 1, None, {})
            raised = False
        except ValueError:
            raised = True
    assert raised, "there is no event type for controlling equipment"


def test_full_run_verifies(pipeline, clock, tmp_path):
    feed(pipeline, clock, 60)
    pipeline._maybe_checkpoint(force=True)
    rep = verify_database(pipeline.db.writer, pipeline.hasher, tmp_path / "checkpoints.jsonl")
    assert rep.ok, rep.issues
    assert rep.windows_checked == 60 and rep.decisions_checked == 60
    assert rep.checkpoints_checked >= 2 and rep.head_seq == pipeline.chain.head[0]
