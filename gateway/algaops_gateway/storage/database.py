"""SQLite connection management and schema migrations.

WAL journal + synchronous=FULL: a committed transaction (window, decision,
alert change and audit events of one message) survives power loss, and a
crash mid-transaction leaves no partial rows.  Writes happen only on the
pipeline worker thread; API handlers use separate read-only connections.
"""

from __future__ import annotations

import contextlib
import sqlite3
import threading
import time
from importlib import resources
from pathlib import Path
from typing import Iterator

MIGRATIONS_PACKAGE = "algaops_gateway.storage.migrations"


def now_ms() -> int:
    return time.time_ns() // 1_000_000


def _migration_scripts() -> list[tuple[int, str]]:
    out = []
    for entry in resources.files(MIGRATIONS_PACKAGE).iterdir():
        name = entry.name
        if name.endswith(".sql") and name[:4].isdigit():
            out.append((int(name[:4]), entry.read_text(encoding="utf-8")))
    return sorted(out)


class Database:
    def __init__(self, path: str | Path, durable: bool = True) -> None:
        """durable=False (offline evaluation / scratch databases only) disables fsync."""
        self.path = Path(path)
        self.durable = durable
        if str(self.path) != ":memory:":
            self.path.parent.mkdir(parents=True, exist_ok=True)
        self._local = threading.local()
        self.writer = self._connect(readonly=False)
        self.migrate()

    def _connect(self, readonly: bool) -> sqlite3.Connection:
        if readonly:
            uri = f"{self.path.resolve().as_uri()}?mode=ro"
            conn = sqlite3.connect(uri, uri=True, isolation_level=None, check_same_thread=False,
                                   timeout=10.0)
        else:
            conn = sqlite3.connect(str(self.path), isolation_level=None, check_same_thread=False,
                                   timeout=10.0)
            conn.execute("PRAGMA journal_mode=WAL")
            conn.execute("PRAGMA synchronous=FULL" if self.durable else "PRAGMA synchronous=OFF")
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA foreign_keys=ON")
        conn.execute("PRAGMA busy_timeout=10000")
        return conn

    def connect_readonly(self) -> sqlite3.Connection:
        """New private read-only connection (caller closes it)."""
        return self._connect(readonly=True)

    def reader(self) -> sqlite3.Connection:
        """Per-thread read-only connection (API handlers, exports)."""
        conn = getattr(self._local, "reader", None)
        if conn is None:
            conn = self._connect(readonly=True)
            self._local.reader = conn
        return conn

    def migrate(self) -> list[int]:
        c = self.writer
        c.execute("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, "
                  "applied_ms INTEGER NOT NULL)")
        done = {r[0] for r in c.execute("SELECT version FROM schema_migrations")}
        applied = []
        for version, script in _migration_scripts():
            if version in done:
                continue
            with self.transaction() as tx:
                for stmt in _split_sql(script):
                    tx.execute(stmt)
                tx.execute("INSERT INTO schema_migrations (version, applied_ms) VALUES (?, ?)",
                           (version, now_ms()))
            applied.append(version)
        return applied

    @contextlib.contextmanager
    def transaction(self) -> Iterator[sqlite3.Connection]:
        c = self.writer
        c.execute("BEGIN IMMEDIATE")
        try:
            yield c
        except BaseException:
            c.execute("ROLLBACK")
            raise
        else:
            c.execute("COMMIT")

    def close(self) -> None:
        with contextlib.suppress(Exception):
            self.writer.close()
        conn = getattr(self._local, "reader", None)
        if conn is not None:
            with contextlib.suppress(Exception):
                conn.close()


def _split_sql(script: str) -> list[str]:
    """Split a migration script into statements (no semicolons inside literals)."""
    lines = [ln for ln in script.splitlines() if not ln.strip().startswith("--")]
    return [s.strip() for s in "\n".join(lines).split(";") if s.strip()]
