"""Single writer: while the gateway has its database open, the commands that would append
to its audit chain (prune, baseline import|activate, replay into it) are refused with
exit status 2 instead of forking the chain head; read-only commands keep working; the
gateway does not start while a write command runs; a killed holder frees the lock."""

import json
import os
import sqlite3
import subprocess
import sys

import pytest
from gwtest import feed, wait_for

from algaops_gateway.app import GatewayApp
from algaops_gateway.audit.checkpoint import CheckpointWriter
from algaops_gateway.cli import main as gateway_cli
from algaops_gateway.config import GATEWAY_DIR, REPO_ROOT, load_config
from algaops_gateway.pipeline import Pipeline
from algaops_gateway.storage.database import Database
from algaops_gateway.storage.writer_lock import WriterLock, WriterLockHeld

BASELINE_CSV = REPO_ROOT / "algaops-assurance" / "data" / "v2" / "baseline_modes.csv"
FIXTURE = REPO_ROOT / "simulator" / "fixtures" / "normal_steady.jsonl"
IMPORT = ("baseline", "import", "--node", "node-a", "--csv", str(BASELINE_CSV))


@pytest.fixture
def site(lib, policy, tmp_path, clock):
    """A stopped gateway: database with 2026-01 history (older than 30 days for the CLI,
    which uses real time) and its config, MQTT and API off."""
    db = Database(tmp_path / "gw.db")
    p = Pipeline(lib=lib, policy=policy, db=db, gateway_id="test-gw", clock=clock,
                 checkpoint_writer=CheckpointWriter(tmp_path / "checkpoints.jsonl"))
    p.open()
    feed(p, clock, 20)
    p.close()
    db.close()
    cfg = tmp_path / "gateway.toml"
    cfg.write_text('gateway_id = "test-gw"\n[mqtt]\nenabled = false\n[api]\nenabled = false\n'
                   '[engine]\ntick_interval_s = 0.1\n[logging]\nlevel = "WARNING"\n'
                   '[storage]\ndb_path = "gw.db"\ncheckpoint_file = "checkpoints.jsonl"\n'
                   'export_dir = "exports"\nretention_days = 30\n', encoding="utf-8")
    return cfg


@pytest.fixture
def gateway(site, tmp_path):
    """The gateway running on `site` (real clock).  Its first tick reports the silent
    node's stream timeout; after that it appends nothing by itself."""
    g = GatewayApp(load_config(site))
    g.start()
    try:
        assert wait_for(lambda: "stream.timeout" in types(tmp_path), 10)
        yield g
    finally:
        g.stop()


def cli(site, *argv) -> int:
    return gateway_cli(["--config", str(site), *argv])


def chain(tmp_path) -> list[tuple]:
    conn = sqlite3.connect(f"{(tmp_path / 'gw.db').resolve().as_uri()}?mode=ro", uri=True)
    try:
        return [tuple(r) for r in conn.execute("SELECT seq, type, hash FROM audit_events "
                                               "ORDER BY seq")]
    finally:
        conn.close()


def types(tmp_path) -> list[str]:
    return [t for _, t, _ in chain(tmp_path)]


def free(db) -> bool:
    lock = WriterLock(db)
    try:
        lock.acquire()
    except WriterLockHeld:
        return False
    lock.release()
    return True


def test_write_commands_are_refused_while_the_gateway_runs(site, gateway, tmp_path, capsys):
    before = chain(tmp_path)
    for argv in (("prune",), ("prune", "--days", "30"), IMPORT,
                 ("baseline", "activate", "--id", "1"),
                 ("replay", str(FIXTURE), "--db", str(tmp_path / "gw.db"))):
        assert cli(site, *argv) == 2, argv
        err = capsys.readouterr().err
        assert "refused: " in err and "is already open for writing" in err, err
        assert f"held by pid {os.getpid()}" in err and "stop the gateway first" in err, err
    assert chain(tmp_path) == before, "a refused command appends nothing"
    # the gateway's head is still the chain head: its next append succeeds
    assert gateway.pipeline.record_audit("audit.verified", None, {"ok": True}) == \
        before[-1][0] + 1

    gateway.stop()                                     # e.g. systemctl stop algaops-gateway
    assert cli(site, *IMPORT) == 0
    ids = json.loads(capsys.readouterr().out)["baseline_ids"]
    assert ids and cli(site, "baseline", "activate", "--id", str(ids[0])) == 0
    capsys.readouterr()
    assert cli(site, "prune") == 0
    assert json.loads(capsys.readouterr().out)["pruned"]["windows"] > 0
    assert cli(site, "verify") == 0
    ev = types(tmp_path)
    assert ev.count("baseline.imported") == len(ids) and "archive.created" in ev


def test_read_only_commands_work_while_the_gateway_runs(site, gateway, tmp_path, capsys):
    before = chain(tmp_path)
    assert cli(site, "verify") == 0
    assert json.loads(capsys.readouterr().out)["ok"]
    out = tmp_path / "evidence.zip"
    assert cli(site, "export", "--out", str(out)) == 0 and out.is_file()
    capsys.readouterr()
    assert cli(site, "prune", "--dry-run") == 0
    dry = json.loads(capsys.readouterr().out)
    assert dry["dry_run"] and dry["rows"]["windows"] > 0
    assert cli(site, "baseline", "list") == 0
    assert cli(site, "policy") == 0
    assert chain(tmp_path) == before


def test_the_gateway_does_not_start_while_a_write_command_runs(site, lib, policy, tmp_path,
                                                              capsys):
    db = Database(tmp_path / "gw.db")
    command = Pipeline(lib=lib, policy=policy, db=db, gateway_id="test-gw")   # e.g. a prune
    command.open()
    try:
        before = chain(tmp_path)
        g = GatewayApp(load_config(site))
        with pytest.raises(WriterLockHeld):
            g.start()
        g.stop()                            # a refused start writes nothing, not even on stop
        assert cli(site, "run") == 2
        err = capsys.readouterr().err
        assert "refused: " in err and "the gateway can start once" in err, err
        assert chain(tmp_path) == before
    finally:
        command.close()
        db.close()
    g = GatewayApp(load_config(site))                # the write command has finished
    g.start()
    g.stop()
    ev = types(tmp_path)
    assert ev.count("gateway.started") == ev.count("gateway.stopped") == 3
    assert ev[-1] == "gateway.stopped"


def test_a_killed_gateway_process_releases_the_database(site, tmp_path, capsys):
    """`run` in another process holds the lock; killed (no clean stop), the operating
    system releases it and a write command runs."""
    env = dict(os.environ, PYTHONPATH=os.pathsep.join(
        p for p in (str(GATEWAY_DIR), os.environ.get("PYTHONPATH")) if p))
    log = tmp_path / "gateway.log"
    with open(log, "wb") as out:
        proc = subprocess.Popen([sys.executable, "-m", "algaops_gateway", "--config", str(site),
                                 "run"], stdout=out, stderr=subprocess.STDOUT, env=env)
    try:
        assert wait_for(lambda: types(tmp_path).count("gateway.started") == 2 or
                        proc.poll() is not None, 60)
        assert proc.poll() is None, log.read_text(errors="replace")
        with pytest.raises(WriterLockHeld) as held:
            WriterLock(tmp_path / "gw.db").acquire()
        holder = held.value.holder
        assert holder["pid"] != os.getpid() and holder["command"].endswith(" run")
        assert cli(site, "prune") == 2
        assert f"held by pid {holder['pid']}" in capsys.readouterr().err
    finally:
        proc.kill()
        proc.wait(30)
    assert wait_for(lambda: free(tmp_path / "gw.db"), 10), "released by the operating system"
    assert cli(site, "prune") == 0
    assert json.loads(capsys.readouterr().out)["ok"]
    assert cli(site, "verify") == 0
