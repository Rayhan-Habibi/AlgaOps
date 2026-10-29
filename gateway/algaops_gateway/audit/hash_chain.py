"""Append-only Ascon-Hash256 audit chain stored in SQLite.

Events are appended inside the caller's write transaction, so a decision and
its audit event are committed atomically.  The in-memory head is advanced
only after the transaction commits (commit()/rollback() keep it consistent).
"""

from __future__ import annotations

import sqlite3
import threading
from dataclasses import dataclass
from typing import Any

from .ascon import ZERO_HASH, Hasher
from .canonical import canonical_json

EVENT_TYPES = frozenset({
    "gateway.started", "gateway.stopped",
    "window.diagnosed", "stream.timeout", "ingest.rejected",
    "alert.opened", "alert.cleared", "alert.changed",
    "baseline.captured", "baseline.activated", "baseline.retired", "baseline.imported",
    "feedback.recorded", "export.created", "audit.verified", "node.status", "archive.created",
})


@dataclass(frozen=True)
class AuditEvent:
    seq: int
    ts_ms: int
    type: str
    node_id: str | None
    payload: str
    prev_hash: str
    hash: str

    def as_export_line(self) -> dict[str, Any]:
        return {"seq": self.seq, "ts_ms": self.ts_ms, "type": self.type, "node_id": self.node_id,
                "payload": self.payload, "prev_hash": self.prev_hash, "hash": self.hash}


class AuditChain:
    def __init__(self, hasher: Hasher, gateway_id: str) -> None:
        self._hasher = hasher
        self._gateway_id = gateway_id
        self._lock = threading.Lock()
        self._head_seq = 0
        self._head_hash = ZERO_HASH
        self._pending: list[AuditEvent] = []

    # ---- head management ------------------------------------------------------
    def load_head(self, conn: sqlite3.Connection) -> None:
        row = conn.execute("SELECT seq, hash FROM audit_events ORDER BY seq DESC LIMIT 1").fetchone()
        with self._lock:
            self._pending.clear()
            if row is None:
                self._head_seq, self._head_hash = 0, ZERO_HASH
            else:
                self._head_seq, self._head_hash = int(row["seq"]), bytes.fromhex(row["hash"])

    @property
    def head(self) -> tuple[int, str]:
        with self._lock:
            return self._head_seq, self._head_hash.hex()

    # ---- append -------------------------------------------------------------------
    def append(self, conn: sqlite3.Connection, event_type: str, ts_ms: int,
               node_id: str | None, body: dict[str, Any]) -> AuditEvent:
        """Append within an open transaction; call commit()/rollback() afterwards."""
        if event_type not in EVENT_TYPES:
            raise ValueError(f"unknown audit event type {event_type!r}")
        with self._lock:
            if self._pending:
                prev_seq, prev_hash = self._pending[-1].seq, bytes.fromhex(self._pending[-1].hash)
            else:
                prev_seq, prev_hash = self._head_seq, self._head_hash
            seq = prev_seq + 1
            doc = dict(body)
            doc.update({"type": event_type, "ts_ms": int(ts_ms), "node_id": node_id,
                        "gateway_id": self._gateway_id, "seq": seq})
            payload = canonical_json(doc)
            digest = self._hasher.link(prev_hash, seq, payload.encode("utf-8"))
            ev = AuditEvent(seq, int(ts_ms), event_type, node_id, payload, prev_hash.hex(),
                            digest.hex())
            conn.execute(
                "INSERT INTO audit_events (seq, ts_ms, type, node_id, payload, prev_hash, hash) "
                "VALUES (?, ?, ?, ?, ?, ?, ?)",
                (ev.seq, ev.ts_ms, ev.type, ev.node_id, ev.payload, ev.prev_hash, ev.hash),
            )
            self._pending.append(ev)
            return ev

    def commit(self) -> list[AuditEvent]:
        with self._lock:
            done = self._pending
            if done:
                self._head_seq = done[-1].seq
                self._head_hash = bytes.fromhex(done[-1].hash)
            self._pending = []
            return done

    def rollback(self) -> None:
        with self._lock:
            self._pending = []


def iter_events(conn: sqlite3.Connection, after_seq: int = 0, limit: int | None = None):
    sql = ("SELECT seq, ts_ms, type, node_id, payload, prev_hash, hash FROM audit_events "
           "WHERE seq > ? ORDER BY seq")
    args: tuple = (after_seq,)
    if limit is not None:
        sql += " LIMIT ?"
        args = (after_seq, int(limit))
    for r in conn.execute(sql, args):
        yield AuditEvent(int(r["seq"]), int(r["ts_ms"]), r["type"], r["node_id"], r["payload"],
                         r["prev_hash"], r["hash"])
