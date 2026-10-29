"""Writer lock (storage/writer_lock.py): one holder per database, within one process
too; a refused attempt names the holder; the lock file stays."""

import json
import os

import pytest

from algaops_gateway.storage.writer_lock import WriterLock, WriterLockHeld, lock_path


def test_the_lock_file_lies_next_to_the_database(tmp_path):
    assert lock_path(tmp_path / "gw.db") == tmp_path.resolve() / "gw.db.writer.lock"


def test_one_holder_at_a_time_and_a_refusal_names_it(tmp_path):
    db = tmp_path / "gw.db"
    with WriterLock(db) as holder:
        with pytest.raises(WriterLockHeld) as refused:
            WriterLock(db).acquire()
        assert holder.held, "a refused attempt leaves the holder alone"
    assert not holder.held
    assert refused.value.holder["pid"] == os.getpid()
    msg = str(refused.value)
    assert str(db.resolve()) in msg and str(lock_path(db)) in msg and f"pid {os.getpid()}" in msg
    again = WriterLock(db)
    again.acquire()                     # released: the next holder gets it
    again.release()
    again.release()                     # a second release does nothing
    assert lock_path(db).is_file(), "never deleted: another process may be locking it"


def test_the_lock_file_names_its_holder(tmp_path):
    with WriterLock(tmp_path / "gw.db") as lock:
        line = json.loads(lock.path.read_text(encoding="utf-8"))
    assert line["pid"] == os.getpid() and line["since"].endswith("Z") and line["command"]


def test_an_in_memory_database_has_no_lock(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    with WriterLock(":memory:"), WriterLock(":memory:"):
        pass
    assert not list(tmp_path.iterdir())
