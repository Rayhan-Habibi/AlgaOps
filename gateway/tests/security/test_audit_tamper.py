"""Tamper detection on the live database: every manipulation class is reported."""

import pytest
from gwtest import feed

from algaops_gateway.audit.checkpoint import read_checkpoint_file
from algaops_gateway.audit.verify import verify_database


@pytest.fixture
def run(pipeline, clock, tmp_path):
    feed(pipeline, clock, 40)
    feed(pipeline, clock, 4, start_seq=41, self_test="fail")
    pipeline._maybe_checkpoint(force=True)
    rep = verify_database(pipeline.db.writer, pipeline.hasher, tmp_path / "checkpoints.jsonl")
    assert rep.ok, rep.issues
    return pipeline, tmp_path / "checkpoints.jsonl"


def kinds(pipeline, cp):
    rep = verify_database(pipeline.db.writer, pipeline.hasher, cp)
    assert not rep.ok
    return {i.kind for i in rep.issues}


def test_modified_event_payload(run):
    p, cp = run
    p.db.writer.execute("UPDATE audit_events SET payload = replace(payload, 'CHECK_SENSOR', "
                        "'NORMAL') WHERE seq = (SELECT MAX(seq) - 3 FROM audit_events)")
    assert "HASH_MISMATCH" in kinds(p, cp)


def test_modified_event_with_recomputed_hash_breaks_link(run):
    p, cp = run
    w = p.db.writer
    seq = w.execute("SELECT MAX(seq) - 5 FROM audit_events").fetchone()[0]
    row = w.execute("SELECT prev_hash, payload FROM audit_events WHERE seq = ?", (seq,)).fetchone()
    forged = row["payload"].replace('"trust_score":100', '"trust_score":99')
    h = p.hasher.link(bytes.fromhex(row["prev_hash"]), seq, forged.encode()).hex()
    w.execute("UPDATE audit_events SET payload = ?, hash = ? WHERE seq = ?", (forged, h, seq))
    assert "BROKEN_LINK" in kinds(p, cp)


def test_deleted_event(run):
    p, cp = run
    p.db.writer.execute("DELETE FROM audit_events WHERE seq = 10")
    assert {"SEQUENCE_GAP", "BROKEN_LINK"} & kinds(p, cp)


def test_reordered_events(run):
    p, cp = run
    w = p.db.writer
    a = w.execute("SELECT payload, hash, prev_hash FROM audit_events WHERE seq = 12").fetchone()
    b = w.execute("SELECT payload, hash, prev_hash FROM audit_events WHERE seq = 13").fetchone()
    w.execute("UPDATE audit_events SET payload = ?, hash = 'x' || hash WHERE seq = 12",
              (b["payload"],))
    w.execute("UPDATE audit_events SET payload = ? WHERE seq = 13", (a["payload"],))
    assert {"HASH_MISMATCH", "METADATA_MISMATCH", "BROKEN_LINK"} & kinds(p, cp)


def test_edited_window_row(run):
    p, cp = run
    p.db.writer.execute("UPDATE windows SET record_json = replace(record_json, '\"temp_c\":31',"
                        " '\"temp_c\":21') WHERE id = 5")
    assert "ROW_MODIFIED" in kinds(p, cp)


def test_edited_indexed_column(run):
    p, cp = run
    p.db.writer.execute("UPDATE windows SET vib_rms_g = 9.9 WHERE id = 7")
    assert "ROW_MODIFIED" in kinds(p, cp)


def test_deleted_decision_row(run):
    p, cp = run
    w = p.db.writer
    w.execute("PRAGMA foreign_keys=OFF")
    w.execute("DELETE FROM diagnostics WHERE id = 3")
    assert "MISSING_ROW" in kinds(p, cp)


def test_inserted_unaudited_row(run):
    p, cp = run
    w = p.db.writer
    w.execute("INSERT INTO windows (node_id, boot_id, seq, t_end_ms, received_ms, data_source, "
              "payload_digest, record_json, record_digest) VALUES ('node-a', 7, 999, 1, 1, "
              "'hardware', 'x', '{}', 'y')")
    assert "UNREFERENCED_ROW" in kinds(p, cp)


def test_truncation_below_external_checkpoint(run):
    p, cp = run
    cps = read_checkpoint_file(cp).checkpoints
    last = cps[-1].seq
    w = p.db.writer
    w.execute("DELETE FROM audit_events WHERE seq > ?", (last - 4,))
    w.execute("DELETE FROM checkpoints")          # attacker also wipes the DB copy
    assert {"TRUNCATED"} & kinds(p, cp), "external checkpoint file still reveals truncation"


def test_checkpoint_hash_mismatch(run):
    p, cp = run
    p.db.writer.execute("UPDATE checkpoints SET hash = '00' || substr(hash, 3)")
    assert "CHECKPOINT_MISMATCH" in kinds(p, cp)


def test_reference_out_of_id_order_is_checked_too(run):
    """Verification meets the rows in id order; an appended event naming an earlier
    window (whoever can append can forge one) is looked up and checked like any other."""
    p, cp = run
    p.record_audit("node.status", None, {"window_id": 3, "window_digest": "f" * 64})
    assert "ROW_MODIFIED" in kinds(p, cp)


def test_payload_that_is_not_an_object(run):
    p, cp = run
    p.db.writer.execute("UPDATE audit_events SET payload = '[1]' WHERE seq = 7")
    assert {"BAD_PAYLOAD", "HASH_MISMATCH"} <= kinds(p, cp)


def test_chain_issues_are_listed_before_row_issues(run):
    p, cp = run
    w = p.db.writer
    w.execute("UPDATE windows SET vib_rms_g = 9.9")
    w.execute("DELETE FROM audit_events WHERE seq = (SELECT MAX(seq) - 1 FROM audit_events)")
    rep = verify_database(w, p.hasher, cp, max_issues=3)
    assert not rep.ok and len(rep.issues) == 3
    assert [i.kind for i in rep.issues[:2]] == ["SEQUENCE_GAP", "BROKEN_LINK"]
