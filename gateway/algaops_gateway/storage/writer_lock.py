"""Single-writer lock: one pipeline at a time appends to a database's audit chain.

Inside a gateway every write happens on the pipeline worker thread.  A second
pipeline on the same database (`prune` or `baseline import` run next to the
gateway) would load the same chain head and fork it: the gateway's next append
fails on the audit_events primary key, and so does every message after it until
a restart.  Pipeline.open() therefore takes an exclusive lock on
`<database>.writer.lock`, next to the database, without waiting; close()
releases it, and the operating system releases it when the process ends (killed
or crashed included).  Readers (API handlers, `verify`, `export`,
`prune --dry-run`) never take it.

flock() on POSIX and LockFileEx() (msvcrt.locking) on Windows both refuse a
second descriptor of the same process as well, so a restart within one process
works and a test can hold the lock against a command.  Windows locks are
mandatory: the locked byte lies past the line that names the holder, which a
refused process reads for its message.  The file is never deleted: a deleted
lock file would let two processes lock two different files.
"""

from __future__ import annotations

import contextlib
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

if os.name == "nt":
    import msvcrt
else:
    import fcntl

SUFFIX = ".writer.lock"
_WIN_OFFSET = 1 << 20           # the byte locked on Windows, past the holder line


class WriterLockHeld(RuntimeError):
    """Another pipeline, in this or another process, has the database open for writing."""

    def __init__(self, message: str, holder: dict[str, Any] | None) -> None:
        super().__init__(message)
        self.holder = holder        # {"pid", "since", "command"} if the holder line was readable


def lock_path(db_path: str | Path) -> Path:
    p = Path(db_path).resolve()
    return p.with_name(p.name + SUFFIX)


class WriterLock:
    """Exclusive lock for the writer of one database, taken without waiting (no-op for
    ":memory:", which no other connection can open)."""

    def __init__(self, db_path: str | Path) -> None:
        self.path = None if str(db_path) == ":memory:" else lock_path(db_path)
        self._fd: int | None = None

    @property
    def held(self) -> bool:
        return self._fd is not None

    def acquire(self) -> None:
        """Take the lock or raise WriterLockHeld, naming the holder."""
        if self.path is None or self._fd is not None:
            return
        fd = os.open(self.path, os.O_RDWR | os.O_CREAT | getattr(os, "O_BINARY", 0), 0o644)
        try:
            if not _try_lock(fd):
                holder = _read_holder(fd)
                raise WriterLockHeld(_held_message(self.path, holder), holder)
        except BaseException:
            os.close(fd)
            raise
        self._fd = fd
        _write_holder(fd)

    def release(self) -> None:
        fd, self._fd = self._fd, None
        if fd is None:
            return
        try:
            _unlock(fd)
        finally:
            os.close(fd)

    def __enter__(self) -> WriterLock:
        self.acquire()
        return self

    def __exit__(self, *exc: object) -> None:
        self.release()


def _try_lock(fd: int) -> bool:
    try:
        if os.name == "nt":
            os.lseek(fd, _WIN_OFFSET, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except (BlockingIOError, PermissionError):      # POSIX EWOULDBLOCK, Windows lock violation
        return False
    return True


def _unlock(fd: int) -> None:
    if os.name == "nt":
        os.lseek(fd, _WIN_OFFSET, os.SEEK_SET)
        msvcrt.locking(fd, msvcrt.LK_UNLCK, 1)
    else:
        fcntl.flock(fd, fcntl.LOCK_UN)


def _write_holder(fd: int) -> None:
    """One JSON line naming the holder, for the refusal message (the lock does not depend
    on it, so a failed write is ignored)."""
    line = json.dumps({"pid": os.getpid(),
                       "since": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                       "command": " ".join(sys.argv)[:300]}) + "\n"
    with contextlib.suppress(OSError):
        os.ftruncate(fd, 0)
        os.lseek(fd, 0, os.SEEK_SET)
        os.write(fd, line.encode("utf-8"))


def _read_holder(fd: int) -> dict[str, Any] | None:
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        holder = json.loads(os.read(fd, 4096).decode("utf-8"))
    except (OSError, ValueError):       # being written right now, or not written by us
        return None
    return holder if isinstance(holder, dict) and "pid" in holder else None


def _held_message(path: Path, holder: dict[str, Any] | None) -> str:
    db = path.with_name(path.name[:-len(SUFFIX)])
    who = (f"pid {holder['pid']} (since {holder.get('since')}: {holder.get('command')})"
           if holder else "another process")
    return f"{db} is already open for writing: its writer lock {path} is held by {who}"
