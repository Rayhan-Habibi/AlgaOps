"""CSV exports of history tables (windows, diagnostics, alerts) for analysis."""

from __future__ import annotations

import csv
import io
import json
import sqlite3
from collections.abc import Iterator

TABLES = {
    "windows": ("received_ms",
                ["id", "node_id", "boot_id", "seq", "t_end_ms", "received_ms", "data_source",
                 "vib_rms_g", "temp_c", "current_a", "record_digest"]),
    "diagnostics": ("decided_ms",
                    ["id", "window_id", "node_id", "kind", "decided_ms", "outcome",
                     "trust_score", "is_trusted", "flags", "mode", "method", "alert_event",
                     "latency_ms", "decision_digest", "audit_seq"]),
    "alerts": ("opened_ms",
               ["id", "node_id", "kind", "status", "opened_ms", "closed_ms", "summary"]),
}


def export_csv(conn: sqlite3.Connection, table: str, node_id: str | None = None,
               since_ms: int | None = None, until_ms: int | None = None) -> Iterator[str]:
    if table not in TABLES:
        raise ValueError(f"unknown table {table!r}")
    time_col, cols = TABLES[table]
    clauses, args = [], []
    if node_id:
        clauses.append("node_id = ?")
        args.append(node_id)
    if since_ms is not None:
        clauses.append(f"{time_col} >= ?")
        args.append(since_ms)
    if until_ms is not None:
        clauses.append(f"{time_col} <= ?")
        args.append(until_ms)
    where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
    buf = io.StringIO()
    w = csv.writer(buf, lineterminator="\n")
    extra = ["flag_names", "reason"] if table == "diagnostics" else []
    w.writerow(cols + extra)
    yield buf.getvalue()
    sel = ", ".join(cols + (["decision_json"] if extra else []))
    for r in conn.execute(f"SELECT {sel} FROM {table}{where} ORDER BY id", args):
        buf.seek(0)
        buf.truncate()
        row = [r[c] for c in cols]
        if extra:
            doc = json.loads(r["decision_json"])
            row += ["|".join(doc["trust"]["flags"]), doc["reason"]]
        w.writerow(row)
        yield buf.getvalue()
