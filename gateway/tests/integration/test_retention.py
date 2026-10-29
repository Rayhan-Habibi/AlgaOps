"""Retention: archive, then prune.  Verification stays meaningful afterwards: the
archived rows are accepted, everything else is checked as before."""

import csv
import io
import json
import sqlite3
import zipfile
from pathlib import Path

import pytest
import verify_evidence
from gwtest import T0, TOPIC, make_doc, wait_for

from algaops_gateway import retention
from algaops_gateway.audit.checkpoint import CheckpointWriter
from algaops_gateway.audit.verify import verify_database
from algaops_gateway.cli import main as gateway_cli
from algaops_gateway.export.evidence_package import build_bundle
from algaops_gateway.pipeline import Pipeline
from algaops_gateway.storage.database import Database

DAY = retention.DAY_MS
NEW = 40 * DAY                    # the second day of data; 30-day retention keeps only it
TABLES = ("windows", "diagnostics", "gaps", "ingest_log", "alerts", "baselines", "feedback",
          "nodes", "audit_events", "checkpoints")


def send(pipe, clock, seq, shift=0, boot_id=7, **over):
    d = make_doc(seq, boot_id=boot_id, **over)
    d["t_end_ms"] += shift
    d["t_start_ms"] += shift
    clock.t = d["t_end_ms"] + 50
    pipe.handle_message(TOPIC, json.dumps(d).encode(), clock.t)
    return d


def history(pipe, clock):
    """Two days 40 days apart (new boot), each with a rejected message, an exact
    redelivery, a sequence gap and two untrusted spells.  The first spell's alert clears
    during start-up (NORMAL windows); the second one stays open in steady operation
    (NOT_EVALUATED without a baseline does not clear), on the old day until the start-up
    of the new day.  Feedback on the first alert."""
    for shift, boot in ((0, 7), (NEW, 8)):
        for s in range(1, 41):
            last = send(pipe, clock, s, shift, boot,
                        **({"samples_valid": 10} if s in (3, 4, 5, 6, 31, 32, 33, 34) else {}))
        pipe.handle_message(TOPIC, b'{"schema": "nope"}', clock.t + 10)        # rejected
        pipe.handle_message(TOPIC, json.dumps(last).encode(), clock.t + 20)   # redelivery
        for s in range(43, 46):                                        # 41, 42 lost: gap
            send(pipe, clock, s, shift, boot)
    alert = pipe.db.writer.execute("SELECT id FROM alerts ORDER BY id").fetchone()[0]
    pipe.record_feedback(alert, "true_alarm", "loose cable", "JA")
    pipe._maybe_checkpoint(force=True)


def counts(conn):
    return {t: conn.execute(f"SELECT COUNT(*) FROM {t}").fetchone()[0] for t in TABLES}


def ids(conn, table):
    return {r[0] for r in conn.execute(f"SELECT id FROM {table}")}


def expected_removal(conn, cutoff):
    """Independent of retention.py: old decisions minus those an alert names, and their
    windows; old gaps and ingest-log rows."""
    kept = {r[0] for r in conn.execute("SELECT open_diag_id FROM alerts UNION "
                                       "SELECT close_diag_id FROM alerts")}
    diags = {r[0]: r[1] for r in conn.execute("SELECT id, window_id FROM diagnostics "
                                              "WHERE decided_ms < ?", (cutoff,))
             if r[0] not in kept}
    return {"diagnostics": set(diags), "windows": {w for w in diags.values() if w is not None},
            "gaps": {r[0] for r in conn.execute("SELECT id FROM gaps WHERE detected_ms < ?",
                                                (cutoff,))},
            "ingest_log": {r[0] for r in conn.execute("SELECT id FROM ingest_log WHERE "
                                                      "received_ms < ?", (cutoff,))}}


def archive_events(conn):
    return [json.loads(r[0]) for r in conn.execute(
        "SELECT payload FROM audit_events WHERE type = 'archive.created' ORDER BY seq")]


def export(pipe, out, checkpoint_file):
    conn = pipe.db.connect_readonly()
    try:
        build_bundle(conn, pipe.hasher, out, gateway_id="test-gw", engine_version="2.0.0",
                     policy_hash=pipe.policy.hash, created_ms=1, checkpoint_file=checkpoint_file)
    finally:
        conn.close()
    return out


def jsonl_ids(path, name):
    with zipfile.ZipFile(path) as z:
        return {json.loads(line)["id"] for line in z.read(name).splitlines() if line.strip()}


def csv_ids(path, name):
    with zipfile.ZipFile(path) as z:
        rows = list(csv.DictReader(io.StringIO(z.read(name).decode("utf-8"))))
    return {int(r["id"]) for r in rows}


@pytest.fixture
def rpipe(lib, policy, db, clock, tmp_path):
    p = Pipeline(lib=lib, policy=policy, db=db, gateway_id="test-gw", clock=clock,
                 checkpoint_writer=CheckpointWriter(tmp_path / "checkpoints.jsonl"),
                 checkpoint_every_events=50, checkpoint_every_s=3600,
                 retention_days=30, archive_dir=tmp_path / "exports")
    p.open()
    yield p
    p.close()


def test_prune_archives_first_and_verification_stays_ok(rpipe, clock, tmp_path):
    history(rpipe, clock)
    w = rpipe.db.writer
    cutoff = clock.t - 30 * DAY
    expected = expected_removal(w, cutoff)
    before, before_ids = counts(w), {t: ids(w, t) for t in expected}
    res = rpipe.prune_now()
    assert res["ok"], res
    after = counts(w)
    removed = {t: before_ids[t] - ids(w, t) for t in expected}
    assert removed == expected and all(removed.values())
    assert res["pruned"] == {t: len(v) for t, v in expected.items()}
    # the decisions that opened and cleared alert 1 and opened alert 2 (cleared 40 days later)
    assert res["kept_for_alerts"] == 3 and res["cutoff_ms"] == cutoff
    for t in ("alerts", "baselines", "feedback", "nodes"):
        assert after[t] == before[t], t
    assert after["audit_events"] == before["audit_events"] + 1          # archive.created
    assert after["checkpoints"] == before["checkpoints"] + 1            # anchors it
    ev = archive_events(w)[0]
    assert ev["file"] == res["file"] and ev["cutoff_ms"] == cutoff and ev["retention_days"] == 30
    assert ev["prune"] == res["pruned"] and ev["archived_through_seq"] > 0

    rep = verify_database(w, rpipe.hasher, tmp_path / "checkpoints.jsonl")
    assert rep.ok, rep.issues
    assert rep.rows_archived == len(removed["windows"]) + len(removed["diagnostics"])
    assert rep.archives[0]["file"] == res["file"]
    assert any("archived and pruned" in n for n in rep.notes)
    assert rpipe.prune_now()["file"] is None, "a second run finds nothing to do"


def test_archive_bundle_verifies_standalone_and_holds_every_pruned_row(rpipe, clock):
    history(rpipe, clock)
    w = rpipe.db.writer
    expected = expected_removal(w, clock.t - 30 * DAY)
    res = rpipe.prune_now()
    path = Path(res["path"])
    assert path.parent.name == "exports" and path.name.startswith("algaops-evidence-test-gw-arch")
    rep = verify_evidence.verify(path)
    assert rep["verified"], rep["issues"][:5]
    assert expected["windows"] <= jsonl_ids(path, "windows.jsonl")
    assert expected["diagnostics"] <= jsonl_ids(path, "diagnostics.jsonl")
    assert expected["gaps"] <= csv_ids(path, "gaps.csv")
    assert expected["ingest_log"] <= csv_ids(path, "ingest_log.csv")
    with zipfile.ZipFile(path) as z:
        man = json.loads(z.read("manifest.json"))
    assert man["scope"]["until_ms"] == res["cutoff_ms"] - 1 and man["verification"]["ok"]
    ev = archive_events(w)[0]
    assert ev["bundle_digest"] == rpipe.hasher.hexdigest(path.read_bytes())
    assert (ev["bundle_head_seq"], ev["bundle_head_hash"]) == (rep["events"], rep["head_hash"])


def test_bundles_exported_before_and_after_pruning_verify_standalone(rpipe, clock, tmp_path):
    history(rpipe, clock)
    cp = tmp_path / "checkpoints.jsonl"
    before = export(rpipe, tmp_path / "before.zip", cp)
    res = rpipe.prune_now()
    after = export(rpipe, tmp_path / "after.zip", cp)
    for path in (before, after):
        rep = verify_evidence.verify(path)
        assert rep["verified"], (path.name, rep["issues"][:5])
    rep = verify_evidence.verify(after)
    assert [a["file"] for a in rep["archives"]] == [res["file"]]
    assert any("archive.created" in n for n in rep["notes"])
    assert jsonl_ids(after, "windows.jsonl") == ids(rpipe.db.writer, "windows")
    assert not verify_evidence.verify(before)["archives"]


@pytest.mark.parametrize("statement, expected", [
    # a remaining new window / an old decision kept because it opened the first alert
    ("UPDATE windows SET record_json = replace(record_json, '\"temp_c\":31', '\"temp_c\":21') "
     "WHERE id = (SELECT MAX(id) FROM windows)", "ROW_MODIFIED"),
    ("UPDATE diagnostics SET decision_json = replace(decision_json, 'CHECK_SENSOR', 'NORMAL') "
     "WHERE id = (SELECT MIN(id) FROM diagnostics)", "ROW_MODIFIED"),
    ("UPDATE windows SET vib_rms_g = 9.9 WHERE id = (SELECT MIN(id) FROM windows)",
     "ROW_MODIFIED"),
    # deleting rows the archive does not cover
    ("DELETE FROM windows WHERE id = (SELECT MAX(id) FROM windows)", "MISSING_ROW"),
    ("DELETE FROM diagnostics WHERE id = (SELECT MAX(id) FROM diagnostics)", "MISSING_ROW"),
    ("DELETE FROM diagnostics WHERE id = (SELECT MIN(id) FROM diagnostics)", "MISSING_ROW"),
    ("DELETE FROM diagnostics WHERE id = (SELECT close_diag_id FROM alerts WHERE id = 1)",
     "MISSING_ROW"),
    # the chain itself, including the archive event
    ("DELETE FROM audit_events WHERE seq = 5", "SEQUENCE_GAP"),
    ("UPDATE audit_events SET payload = replace(payload, '\"archived_through_seq\":', "
     "'\"archived_through_seq\":9') WHERE type = 'archive.created'", "HASH_MISMATCH"),
    ("DELETE FROM audit_events WHERE type = 'archive.created'", "MISSING_ROW"),
], ids=["new-window", "kept-old-decision", "kept-old-window-column", "deleted-new-window",
        "deleted-new-decision", "deleted-kept-opening-decision", "deleted-kept-clearing-decision",
        "deleted-event", "edited-archive-event", "deleted-archive-event"])
def test_tampering_after_pruning_is_still_detected(rpipe, clock, tmp_path, statement, expected):
    history(rpipe, clock)
    assert rpipe.prune_now()["ok"]
    w = rpipe.db.writer
    w.execute("PRAGMA foreign_keys=OFF")
    w.execute(statement)
    rep = verify_database(w, rpipe.hasher, tmp_path / "checkpoints.jsonl")
    assert not rep.ok and expected in {i.kind for i in rep.issues}, rep.issues[:5]


def test_nothing_is_pruned_when_verification_fails(rpipe, clock, tmp_path):
    history(rpipe, clock)
    w = rpipe.db.writer
    w.execute("UPDATE windows SET vib_rms_g = 9.9 WHERE id = 3")     # an old row, tampered
    before = counts(w)
    res = rpipe.prune_now()
    assert not res["ok"] and "verification" in res["error"]
    assert counts(w) == before and not archive_events(w)
    assert not list((tmp_path / "exports").glob("*")), "the failed archive is not kept"


def test_a_failed_first_batch_changes_nothing_and_leaves_no_archive(rpipe, clock, tmp_path,
                                                                     monkeypatch):
    history(rpipe, clock)
    w = rpipe.db.writer
    before, head = counts(w), rpipe.chain.head

    def disk_full(self, tx):
        tx.execute("DELETE FROM gaps")
        raise OSError("disk full (simulated)")
    monkeypatch.setattr(retention.PruneJob, "_batch", disk_full)
    with pytest.raises(OSError):
        rpipe.prune_now()
    assert counts(w) == before and rpipe.chain.head == head and not archive_events(w)
    assert not list(tmp_path.glob("exports/*"))
    assert verify_database(w, rpipe.hasher, tmp_path / "checkpoints.jsonl").ok


def test_daily_run_from_the_tick(rpipe, clock):
    history(rpipe, clock)                         # the clock is now 40 days after the start
    rpipe.tick(clock.t)
    assert len(archive_events(rpipe.db.writer)) == 1
    rpipe.tick(clock.t + 3600_000)                # a stream timeout, but no second run today
    assert len(archive_events(rpipe.db.writer)) == 1
    assert verify_database(rpipe.db.writer, rpipe.hasher).ok


def test_daily_run_on_the_worker_prunes_in_batches_between_messages(lib, policy, tmp_path,
                                                                    clock, monkeypatch):
    monkeypatch.setattr(retention, "BATCH", 1)
    db = Database(tmp_path / "w.db")
    p = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw", clock=clock,
                 checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"), tick_interval_s=0.05,
                 retention_days=30, archive_dir=tmp_path / "exports")
    p.open()
    history(p, clock)
    expected = expected_removal(db.writer, clock.t - 30 * DAY)
    ro = db.connect_readonly()
    stored = lambda: ro.execute("SELECT COUNT(*) FROM windows WHERE boot_id = 8 AND "  # noqa
                                "seq = 38").fetchone()[0]
    p.start_worker()
    try:
        assert wait_for(lambda: p._prune_job is not None, 20), "archive written, pruning"
        d = make_doc(38, boot_id=8)
        d["t_end_ms"] += NEW
        d["t_start_ms"] += NEW
        assert p.submit_message(TOPIC, json.dumps(d).encode(), clock.t + 1280)
        assert wait_for(stored, 10)
        assert p._prune_job is not None, "the message was stored while pruning went on"
        assert wait_for(lambda: p._prune_job is None and not p._retention_busy, 30)
    finally:
        p.stop()
        ro.close()
    res = archive_events(db.writer)
    assert len(res) == 1 and res[0]["prune"]["diagnostics"] == len(expected["diagnostics"])
    rep = verify_database(db.writer, p.hasher, tmp_path / "cp.jsonl")
    db.close()
    assert rep.ok, rep.issues
    assert rep.rows_archived == len(expected["windows"]) + len(expected["diagnostics"])


def test_an_appended_archive_event_is_listed_for_the_auditor(rpipe, clock, tmp_path):
    """Documented limit: whoever can append to the chain can declare rows archived.
    The forged event is in the chain and in every report, where it has to match a
    bundle kept off-device; a wrong bundle head is reported like a checkpoint."""
    history(rpipe, clock)
    w = rpipe.db.writer
    head = rpipe.chain.head
    rpipe.record_audit("archive.created", None, {
        "cutoff_ms": clock.t + DAY, "archived_through_seq": head[0], "file": "forged.zip",
        "bundle_digest": "0" * 64, "bundle_head_seq": head[0], "bundle_head_hash": head[1]})
    w.execute("PRAGMA foreign_keys=OFF")
    w.execute("DELETE FROM windows WHERE id = (SELECT MAX(id) FROM windows)")
    rep = verify_database(w, rpipe.hasher, tmp_path / "checkpoints.jsonl")
    assert rep.ok and [a["file"] for a in rep.archives] == ["forged.zip"]
    rpipe.record_audit("archive.created", None, {
        "cutoff_ms": 1, "archived_through_seq": 1, "file": "x.zip", "bundle_digest": "0" * 64,
        "bundle_head_seq": head[0], "bundle_head_hash": "f" * 64})
    rep = verify_database(w, rpipe.hasher, tmp_path / "checkpoints.jsonl")
    assert "CHECKPOINT_MISMATCH" in {i.kind for i in rep.issues}


# ---- command line ------------------------------------------------------------------------

@pytest.fixture
def offline(lib, policy, tmp_path, clock):
    """A stopped gateway: database, checkpoint file and config (the CLI uses real time,
    so all of this 2026-01 data is older than 30 days)."""
    db = Database(tmp_path / "gw.db")
    p = Pipeline(lib=lib, policy=policy, db=db, gateway_id="test-gw", clock=clock,
                 checkpoint_writer=CheckpointWriter(tmp_path / "checkpoints.jsonl"))
    p.open()
    history(p, clock)
    p.close()
    db.close()
    cfg = tmp_path / "gateway.toml"
    cfg.write_text('gateway_id = "test-gw"\n[mqtt]\nenabled = false\n[api]\nenabled = false\n'
                   '[storage]\ndb_path = "gw.db"\ncheckpoint_file = "checkpoints.jsonl"\n'
                   'export_dir = "exports"\nretention_days = 30\n', encoding="utf-8")
    return cfg


def state(tmp_path):
    conn = sqlite3.connect(f"{(tmp_path / 'gw.db').resolve().as_uri()}?mode=ro", uri=True)
    try:
        heads = conn.execute("SELECT MAX(seq), (SELECT hash FROM audit_events ORDER BY seq DESC "
                             "LIMIT 1) FROM audit_events").fetchone()
        return (counts(conn), heads, (tmp_path / "checkpoints.jsonl").read_bytes(),
                sorted(p.name for p in tmp_path.glob("exports/*")))
    finally:
        conn.close()


def test_cli_dry_run_changes_nothing(offline, tmp_path, capsys):
    before = state(tmp_path)
    assert gateway_cli(["--config", str(offline), "prune", "--dry-run"]) == 0
    out = json.loads(capsys.readouterr().out)
    assert out["dry_run"] and out["retention_days"] == 30
    assert out["rows"]["diagnostics"] > 0 and out["rows"]["windows"] > 0
    assert state(tmp_path) == before


def test_cli_prune_then_verify(offline, tmp_path, capsys):
    assert gateway_cli(["--config", str(offline), "prune", "--days", "30"]) == 0
    res = json.loads(capsys.readouterr().out)
    assert res["ok"] and res["pruned"]["diagnostics"] > 0
    assert verify_evidence.verify(Path(res["path"]))["verified"]
    assert gateway_cli(["--config", str(offline), "verify"]) == 0
    rep = json.loads(capsys.readouterr().out)
    assert rep["ok"] and rep["rows_archived"] == res["pruned"]["windows"] + \
        res["pruned"]["diagnostics"]


def test_cli_prune_needs_a_retention_period(offline, tmp_path, capsys):
    text = offline.read_text(encoding="utf-8").replace("retention_days = 30", "retention_days = 0")
    offline.write_text(text, encoding="utf-8")
    assert gateway_cli(["--config", str(offline), "prune"]) == 2
    assert "retention_days" in capsys.readouterr().err
