import sqlite3

import pytest

from algaops_gateway.storage.database import Database


def test_migrations_idempotent(tmp_path):
    d = Database(tmp_path / "a.db")
    assert d.migrate() == []
    tables = {r[0] for r in d.writer.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    assert {"windows", "diagnostics", "alerts", "baselines", "feedback", "audit_events",
            "checkpoints", "ingest_log", "gaps", "nodes"} <= tables
    assert d.writer.execute("PRAGMA journal_mode").fetchone()[0].lower() == "wal"
    d.close()
    d2 = Database(tmp_path / "a.db")
    assert d2.migrate() == []
    d2.close()


def test_readonly_connection_cannot_write(db):
    ro = db.connect_readonly()
    with pytest.raises(sqlite3.OperationalError):
        ro.execute("INSERT INTO settings (key, value) VALUES ('a', 'b')")
    ro.close()


def test_transaction_rollback(db):
    with pytest.raises(RuntimeError):
        with db.transaction() as tx:
            tx.execute("INSERT INTO settings (key, value) VALUES ('k', 'v')")
            raise RuntimeError("abort")
    assert db.writer.execute("SELECT COUNT(*) FROM settings").fetchone()[0] == 0


def test_window_uniqueness_constraint(db):
    from algaops_gateway.storage import repositories as repo
    kw = dict(node_id="n", boot_id=1, seq=1, t_end_ms=1, received_ms=1, data_source="simulator",
              payload_digest="d", record_json="{}", record_digest="r", record={})
    with db.transaction() as tx:
        repo.insert_window(tx, **kw)
    with pytest.raises(repo.DuplicateWindow):
        with db.transaction() as tx:
            repo.insert_window(tx, **kw)
