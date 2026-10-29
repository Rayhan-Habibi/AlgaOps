"""Tampering with a copy of the live run's database, checked with the gateway
CLI (`algaops-gateway verify`).  The last test demonstrates the documented
limit: a consistent rewrite of the database and of every checkpoint is only
exposed by a head hash or checkpoint kept somewhere else."""

import json
import shutil
import sqlite3

import pytest

from algaops_gateway.audit.ascon import Hasher
from algaops_gateway.audit.canonical import canonical_json
from algaops_gateway.cli import main as gateway_cli
from algaops_gateway.core.libalgaops import load_library


def snapshot(system, dest, checkpoint_name="checkpoints.jsonl"):
    """Consistent copy: checkpoints first, so the database copy covers all of them."""
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(system.checkpoint_path, dest / "checkpoints.jsonl")
    src = sqlite3.connect(f"file:{system.db_path.as_posix()}?mode=ro", uri=True)
    dst = sqlite3.connect(dest / "copy.db")
    src.backup(dst)
    src.close()
    dst.close()
    return write_config(dest, checkpoint_name), dest / "copy.db"


def write_config(dest, checkpoint_name):
    cfg = dest / f"gateway-{checkpoint_name}.toml"
    cfg.write_text('gateway_id = "e2e-gw"\n[mqtt]\nenabled = false\n[api]\nenabled = false\n'
                   f'[storage]\ndb_path = "copy.db"\ncheckpoint_file = "{checkpoint_name}"\n',
                   encoding="utf-8")
    return cfg


def verify(cfg, capsys):
    rc = gateway_cli(["--config", str(cfg), "verify"])
    rep = json.loads(capsys.readouterr().out)
    return rc, {i["kind"] for i in rep["issues"]}


def edit(db, *statements):
    with sqlite3.connect(db) as c:
        for sql, args in statements:
            c.execute(sql, args)


def first_equipment_decision(db):
    with sqlite3.connect(db) as c:
        return c.execute("SELECT id, decision_json, audit_seq FROM diagnostics WHERE "
                         "node_id = 'e2e-equipment' AND outcome = 'CHECK_EQUIPMENT' "
                         "ORDER BY id LIMIT 1").fetchone()


def test_untouched_copy_verifies(system, tmp_path, capsys):
    cfg, _ = snapshot(system, tmp_path)
    assert verify(cfg, capsys) == (0, set())


def test_edited_decision_is_detected(system, tmp_path, capsys):
    cfg, db = snapshot(system, tmp_path)
    did, dj, _ = first_equipment_decision(db)
    forged = dj.replace('"outcome":"CHECK_EQUIPMENT"', '"outcome":"NORMAL"')
    assert forged != dj
    edit(db, ("UPDATE diagnostics SET outcome = 'NORMAL', decision_json = ? WHERE id = ?",
              (forged, did)))
    rc, kinds = verify(cfg, capsys)
    assert rc == 3 and "ROW_MODIFIED" in kinds


@pytest.mark.parametrize("statements, expected", [
    ([("DELETE FROM audit_events WHERE seq = (SELECT MIN(seq) FROM audit_events "
       "WHERE type = 'alert.opened')", ())], {"SEQUENCE_GAP", "BROKEN_LINK"}),
    ([("DELETE FROM windows WHERE id = (SELECT MIN(id) FROM windows "
       "WHERE node_id = 'e2e-stuck')", ())], {"MISSING_ROW"}),
    ([("INSERT INTO diagnostics (window_id, node_id, kind, decided_ms, ref_ms, outcome, "
       "trust_score, is_trusted, flags, mode, method, alert_event, latency_ms, decision_json, "
       "decision_digest, audit_seq) SELECT window_id, node_id, kind, decided_ms, ref_ms, "
       "'NORMAL', trust_score, is_trusted, flags, mode, method, alert_event, latency_ms, "
       "decision_json, decision_digest, NULL FROM diagnostics ORDER BY id LIMIT 1", ())],
     {"UNREFERENCED_ROW"}),
    ([("UPDATE audit_events SET node_id = 'e2e-normal' WHERE seq = (SELECT MIN(seq) "
       "FROM audit_events WHERE node_id = 'e2e-stuck')", ())], {"METADATA_MISMATCH"}),
], ids=["deleted-event", "deleted-window", "inserted-decision", "relabelled-event"])
def test_structural_tampering_is_detected(system, tmp_path, capsys, statements, expected):
    cfg, db = snapshot(system, tmp_path)
    edit(db, *statements)
    rc, kinds = verify(cfg, capsys)
    assert rc == 3 and kinds & expected, kinds


def test_swapped_events_are_detected(system, tmp_path, capsys):
    cfg, db = snapshot(system, tmp_path)
    with sqlite3.connect(db) as c:
        a, b = c.execute("SELECT seq, payload FROM audit_events WHERE type = 'window.diagnosed' "
                         "ORDER BY seq LIMIT 2").fetchall()
        c.execute("UPDATE audit_events SET payload = ? WHERE seq = ?", (b[1], a[0]))
        c.execute("UPDATE audit_events SET payload = ? WHERE seq = ?", (a[1], b[0]))
    rc, kinds = verify(cfg, capsys)
    assert rc == 3 and "HASH_MISMATCH" in kinds


def test_truncated_tail_is_detected_by_checkpoints(system, tmp_path, capsys):
    cfg, db = snapshot(system, tmp_path)
    last_cp = max(json.loads(line)["seq"] for line in
                  (tmp_path / "checkpoints.jsonl").read_text().splitlines() if line.strip())
    edit(db, ("DELETE FROM audit_events WHERE seq > ?", (last_cp - 3,)),
         ("DELETE FROM checkpoints WHERE seq > ?", (last_cp - 3,)))
    rc, kinds = verify(cfg, capsys)
    assert rc == 3 and "TRUNCATED" in kinds


def test_consistent_rewrite_needs_an_independent_anchor(system, tmp_path, capsys):
    cfg, db = snapshot(system, tmp_path)
    shutil.copyfile(tmp_path / "checkpoints.jsonl", tmp_path / "anchor.jsonl")  # kept elsewhere
    hasher = Hasher(load_library())
    did, dj, aseq = first_equipment_decision(db)
    forged = dj.replace('"outcome":"CHECK_EQUIPMENT"', '"outcome":"NORMAL"')
    digest = hasher.hexdigest(forged.encode("utf-8"))
    with sqlite3.connect(db) as c:
        c.execute("UPDATE diagnostics SET outcome = 'NORMAL', decision_json = ?, "
                  "decision_digest = ? WHERE id = ?", (forged, digest, did))
        body = json.loads(c.execute("SELECT payload FROM audit_events WHERE seq = ?",
                                    (aseq,)).fetchone()[0])
        body.update(decision_digest=digest, outcome="NORMAL")
        c.execute("UPDATE audit_events SET payload = ? WHERE seq = ?", (canonical_json(body), aseq))
        prev = bytes.fromhex(c.execute("SELECT hash FROM audit_events WHERE seq = ?",
                                       (aseq - 1,)).fetchone()[0])
        for seq, payload in c.execute("SELECT seq, payload FROM audit_events WHERE seq >= ? "
                                      "ORDER BY seq", (aseq,)).fetchall():
            h = hasher.link(prev, seq, payload.encode("utf-8"))
            c.execute("UPDATE audit_events SET prev_hash = ?, hash = ? WHERE seq = ?",
                      (prev.hex(), h.hex(), seq))
            prev = h
        c.execute("UPDATE checkpoints SET hash = (SELECT hash FROM audit_events a "
                  "WHERE a.seq = checkpoints.seq)")
        hashes = dict(c.execute("SELECT seq, hash FROM audit_events").fetchall())
    cps = [json.loads(line) for line in (tmp_path / "checkpoints.jsonl").read_text().splitlines()
           if line.strip()]
    (tmp_path / "checkpoints.jsonl").write_text(
        "".join(json.dumps(cp | {"hash": hashes[cp["seq"]]}) + "\n" for cp in cps))

    assert verify(cfg, capsys) == (0, set()), "a full rewrite is self-consistent (limitation)"
    rc, kinds = verify(write_config(tmp_path, "anchor.jsonl"), capsys)
    assert rc == 3 and "CHECKPOINT_MISMATCH" in kinds, "the independent copy exposes it"
