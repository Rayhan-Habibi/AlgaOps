"""JSON Lines export of decisions (full diagnostic_event.v1 documents)."""

from __future__ import annotations

import json
import sqlite3
from collections.abc import Iterator


def export_decisions_jsonl(conn: sqlite3.Connection, node_id: str | None = None,
                           since_ms: int | None = None, until_ms: int | None = None
                           ) -> Iterator[str]:
    clauses, args = [], []
    if node_id:
        clauses.append("node_id = ?")
        args.append(node_id)
    if since_ms is not None:
        clauses.append("decided_ms >= ?")
        args.append(since_ms)
    if until_ms is not None:
        clauses.append("decided_ms <= ?")
        args.append(until_ms)
    where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
    for r in conn.execute(f"SELECT id, window_id, audit_seq, decision_json FROM diagnostics"
                          f"{where} ORDER BY id", args):
        doc = json.loads(r["decision_json"])
        doc.update({"id": r["id"], "window_id": r["window_id"], "audit_seq": r["audit_seq"]})
        yield json.dumps(doc, ensure_ascii=False, separators=(",", ":")) + "\n"
