"""Audit checkpoints: (seq, hash) anchors kept in the DB and in an external
append-only JSONL file (ideally on separate storage, and copied into every
evidence bundle).

A checkpoint is written only after the events it references are committed,
so a checkpoint can never point at an uncommitted event.  A crash while
appending may leave a partial last line; readers skip it and report it.
Truncation of the newest events is detectable only up to the most recent
checkpoint that the attacker could not also rewrite.

A reader that compares the file with a database snapshot reads it only up to
the size it had when the snapshot began (checkpoint_file_size() before the
first query): a line appended later may name an event the snapshot does not
hold, which would look like truncation.
"""

from __future__ import annotations

import json
import os
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator


@dataclass(frozen=True)
class Checkpoint:
    seq: int
    hash: str
    ts_ms: int


@dataclass
class CheckpointFile:
    checkpoints: list[Checkpoint]
    skipped_lines: int


class CheckpointWriter:
    def __init__(self, path: str | Path | None) -> None:
        self.path = Path(path) if path else None
        if self.path:
            self.path.parent.mkdir(parents=True, exist_ok=True)

    def write(self, conn: sqlite3.Connection, cp: Checkpoint) -> None:
        conn.execute("INSERT OR IGNORE INTO checkpoints (seq, hash, ts_ms) VALUES (?, ?, ?)",
                     (cp.seq, cp.hash, cp.ts_ms))
        if self.path:
            line = json.dumps({"seq": cp.seq, "hash": cp.hash, "ts_ms": cp.ts_ms},
                              separators=(",", ":"), sort_keys=True)
            fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o640)
            try:
                os.write(fd, (line + "\n").encode("ascii"))
                os.fsync(fd)
            finally:
                os.close(fd)


def checkpoint_file_size(path: str | Path) -> int:
    """Current size of the external file in bytes (0 if it does not exist)."""
    try:
        return os.stat(path).st_size
    except FileNotFoundError:
        return 0


def iter_checkpoint_file(path: str | Path, size: int | None = None
                         ) -> Iterator[Checkpoint | None]:
    """The external file line by line, without holding it in memory: a Checkpoint per
    readable line, None per unreadable one.  size: only the first `size` bytes."""
    try:
        f = open(path, "rb")
    except FileNotFoundError:
        return
    with f:
        left = size
        for raw in f:
            if left is not None:
                if left <= 0:
                    break
                raw, left = raw[:left], left - len(raw)
            if not raw.strip():
                continue
            try:
                d = json.loads(raw.decode("ascii"))
                yield Checkpoint(int(d["seq"]), str(d["hash"]), int(d["ts_ms"]))
            except (ValueError, KeyError, TypeError, UnicodeDecodeError):
                yield None


def read_checkpoint_file(path: str | Path) -> CheckpointFile:
    cps: list[Checkpoint] = []
    skipped = 0
    for cp in iter_checkpoint_file(path):
        if cp is None:
            skipped += 1
        else:
            cps.append(cp)
    return CheckpointFile(cps, skipped)


def iter_db_checkpoints(conn: sqlite3.Connection) -> Iterator[Checkpoint]:
    for r in conn.execute("SELECT seq, hash, ts_ms FROM checkpoints ORDER BY seq"):
        yield Checkpoint(int(r["seq"]), r["hash"], int(r["ts_ms"]))


def read_db_checkpoints(conn: sqlite3.Connection) -> list[Checkpoint]:
    return list(iter_db_checkpoints(conn))
