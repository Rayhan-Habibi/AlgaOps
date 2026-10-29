"""SQL access layer.  Write helpers are called inside Database.transaction();
read helpers take any connection (normally a per-thread read-only one)."""

from __future__ import annotations

import json
import sqlite3
from typing import Any


class DuplicateWindow(Exception):
    """INSERT hit the (node, boot, seq, payload_digest) uniqueness constraint."""


# ---------------------------------------------------------------- writes ----

def upsert_node(tx: sqlite3.Connection, node_id: str, data_source: str, seen_ms: int,
                boot_id: int | None = None, fw: str | None = None) -> None:
    tx.execute(
        "INSERT INTO nodes (node_id, data_source, first_seen_ms, last_seen_ms, last_boot_id, "
        "fw_version) VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT(node_id) DO UPDATE SET "
        "data_source = excluded.data_source, last_seen_ms = MAX(nodes.last_seen_ms, "
        "excluded.last_seen_ms), last_boot_id = COALESCE(excluded.last_boot_id, "
        "nodes.last_boot_id), fw_version = COALESCE(excluded.fw_version, nodes.fw_version)",
        (node_id, data_source, seen_ms, seen_ms, boot_id, fw))


def update_node_status(tx: sqlite3.Connection, node_id: str, status: dict[str, Any],
                       ts_ms: int, data_source: str) -> str | None:
    row = tx.execute("SELECT status_json FROM nodes WHERE node_id = ?", (node_id,)).fetchone()
    prev = json.loads(row["status_json"]).get("state") if row and row["status_json"] else None
    upsert_node(tx, node_id, data_source, ts_ms, status.get("boot_id"), status.get("fw"))
    tx.execute("UPDATE nodes SET status_json = ?, status_ms = ? WHERE node_id = ?",
               (json.dumps(status, sort_keys=True), ts_ms, node_id))
    return prev


def insert_window(tx: sqlite3.Connection, *, node_id: str, boot_id: int, seq: int,
                  t_end_ms: int, received_ms: int, data_source: str, payload_digest: str,
                  record_json: str, record_digest: str, record: dict[str, Any]) -> int:
    cur = tx.execute(
        "INSERT INTO windows (node_id, boot_id, seq, t_end_ms, received_ms, data_source, "
        "payload_digest, record_json, record_digest, vib_rms_g, temp_c, current_a) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(node_id, boot_id, seq, payload_digest) DO NOTHING",
        (node_id, boot_id, seq, t_end_ms, received_ms, data_source, payload_digest, record_json,
         record_digest, record.get("vib_rms_g"), record.get("temp_c"), record.get("current_a")))
    if cur.rowcount == 0:
        raise DuplicateWindow()
    return int(cur.lastrowid)


def insert_diagnostic(tx: sqlite3.Connection, *, window_id: int | None, doc: dict[str, Any],
                      decision_json: str, decision_digest: str, latency_ms: float | None) -> int:
    trust = doc["trust"]
    ref_ms = doc["timestamp_ms"] or doc["received_ms"]
    cur = tx.execute(
        "INSERT INTO diagnostics (window_id, node_id, kind, decided_ms, ref_ms, outcome, "
        "trust_score, is_trusted, flags, mode, method, alert_event, latency_ms, decision_json, "
        "decision_digest) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        (window_id, doc["node_id"], doc["kind"], doc["decided_ms"], ref_ms, doc["outcome"],
         trust["score"], 1 if trust["trusted"] else 0, trust["flag_bits"], doc["mode"],
         doc["method"], doc["alert"]["event"], latency_ms, decision_json, decision_digest))
    return int(cur.lastrowid)


def set_diagnostic_audit_seq(tx: sqlite3.Connection, diag_id: int, seq: int) -> None:
    tx.execute("UPDATE diagnostics SET audit_seq = ? WHERE id = ?", (seq, diag_id))


def insert_gap(tx: sqlite3.Connection, node_id: str, boot_id: int, seq: int, missing: int,
               detected_ms: int) -> None:
    tx.execute("INSERT INTO gaps (node_id, boot_id, from_seq, to_seq, missing, detected_ms) "
               "VALUES (?, ?, ?, ?, ?, ?)",
               (node_id, boot_id, seq - missing, seq - 1, missing, detected_ms))


def insert_ingest_log(tx: sqlite3.Connection, received_ms: int, topic: str, node_id: str | None,
                      status: str, reason: str | None, raw_digest: str,
                      raw_excerpt: str | None) -> None:
    tx.execute("INSERT INTO ingest_log (received_ms, topic, node_id, status, reason, raw_digest, "
               "raw_excerpt) VALUES (?, ?, ?, ?, ?, ?, ?)",
               (received_ms, topic[:200], node_id, status, reason, raw_digest, raw_excerpt))


def open_alert(tx: sqlite3.Connection, node_id: str, kind: str, opened_ms: int, diag_id: int,
               reason: str) -> int:
    cur = tx.execute("INSERT INTO alerts (node_id, kind, status, opened_ms, open_diag_id, summary, "
                     "last_reason) VALUES (?, ?, 'open', ?, ?, ?, ?)",
                     (node_id, kind, opened_ms, diag_id, reason[:240], reason))
    return int(cur.lastrowid)


def current_open_alert(conn: sqlite3.Connection, node_id: str) -> sqlite3.Row | None:
    return conn.execute("SELECT * FROM alerts WHERE node_id = ? AND status = 'open' "
                        "ORDER BY id DESC LIMIT 1", (node_id,)).fetchone()


def close_alert(tx: sqlite3.Connection, alert_id: int, status: str, closed_ms: int,
                diag_id: int | None) -> None:
    tx.execute("UPDATE alerts SET status = ?, closed_ms = ?, close_diag_id = ? WHERE id = ?",
               (status, closed_ms, diag_id, alert_id))


def touch_alert(tx: sqlite3.Connection, alert_id: int, reason: str) -> None:
    tx.execute("UPDATE alerts SET last_reason = ? WHERE id = ?", (reason, alert_id))


def insert_baseline(tx: sqlite3.Connection, node_id: str, mode: str, created_ms: int, n: int,
                    stats_json: str, stats_digest: str, source: dict[str, Any]) -> int:
    cur = tx.execute("INSERT INTO baselines (node_id, mode, status, created_ms, n, stats_json, "
                     "stats_digest, source_json) VALUES (?, ?, 'candidate', ?, ?, ?, ?, ?)",
                     (node_id, mode, created_ms, n, stats_json, stats_digest,
                      json.dumps(source, sort_keys=True)))
    return int(cur.lastrowid)


def activate_baseline(tx: sqlite3.Connection, baseline_id: int, now_ms: int) -> list[int]:
    row = tx.execute("SELECT node_id, mode FROM baselines WHERE id = ?", (baseline_id,)).fetchone()
    if row is None:
        raise KeyError(baseline_id)
    retired = [int(r[0]) for r in tx.execute(
        "SELECT id FROM baselines WHERE node_id = ? AND mode = ? AND status = 'active' AND id != ?",
        (row["node_id"], row["mode"], baseline_id))]
    tx.execute("UPDATE baselines SET status = 'retired' WHERE node_id = ? AND mode = ? AND "
               "status = 'active' AND id != ?", (row["node_id"], row["mode"], baseline_id))
    tx.execute("UPDATE baselines SET status = 'active', activated_ms = ? WHERE id = ?",
               (now_ms, baseline_id))
    return retired


def retire_baseline(tx: sqlite3.Connection, baseline_id: int) -> None:
    tx.execute("UPDATE baselines SET status = 'retired' WHERE id = ?", (baseline_id,))


def insert_feedback(tx: sqlite3.Connection, alert_id: int, verdict: str, note: str,
                    technician: str, created_ms: int) -> int:
    cur = tx.execute("INSERT INTO feedback (alert_id, verdict, note, technician, created_ms) "
                     "VALUES (?, ?, ?, ?, ?)", (alert_id, verdict, note, technician, created_ms))
    return int(cur.lastrowid)


# ----------------------------------------------------------------- reads ----

def rows(conn: sqlite3.Connection, sql: str, args: tuple = ()) -> list[dict[str, Any]]:
    return [dict(r) for r in conn.execute(sql, args)]


def get_baseline(conn: sqlite3.Connection, baseline_id: int) -> dict[str, Any] | None:
    r = conn.execute("SELECT * FROM baselines WHERE id = ?", (baseline_id,)).fetchone()
    return dict(r) if r else None


def active_baselines(conn: sqlite3.Connection, node_id: str) -> list[dict[str, Any]]:
    return rows(conn, "SELECT * FROM baselines WHERE node_id = ? AND status = 'active'",
                (node_id,))


def list_nodes(conn: sqlite3.Connection) -> list[dict[str, Any]]:
    return rows(conn, "SELECT * FROM nodes ORDER BY node_id")


def latest_decision(conn: sqlite3.Connection, node_id: str) -> dict[str, Any] | None:
    r = conn.execute("SELECT id, window_id, decision_json, audit_seq FROM diagnostics "
                     "WHERE node_id = ? ORDER BY id DESC LIMIT 1", (node_id,)).fetchone()
    if r is None:
        return None
    doc = json.loads(r["decision_json"])
    doc["id"], doc["window_id"], doc["audit_seq"] = r["id"], r["window_id"], r["audit_seq"]
    return doc


def last_decision(conn: sqlite3.Connection, node_id: str) -> sqlite3.Row | None:
    return conn.execute("SELECT id, kind, decided_ms FROM diagnostics WHERE node_id = ? "
                        "ORDER BY id DESC LIMIT 1", (node_id,)).fetchone()


def recent_windows(conn: sqlite3.Connection, node_id: str, limit: int) -> list[sqlite3.Row]:
    out = conn.execute("SELECT id, record_json, payload_digest FROM windows WHERE node_id = ? "
                       "ORDER BY id DESC LIMIT ?", (node_id, limit)).fetchall()
    return list(reversed(out))


def packet_counts(conn: sqlite3.Connection, node_id: str, since_ms: int) -> dict[str, int]:
    """Packet status classes as shown by the dashboard legend."""
    flags = conn.execute(
        "SELECT flags, COUNT(*) AS n FROM diagnostics WHERE node_id = ? AND kind = 'window' "
        "AND decided_ms >= ? GROUP BY flags", (node_id, since_ms)).fetchall()
    from ..core.libalgaops import FLAG_COUNT  # local import to avoid cycles
    names = {}
    counts = {"valid": 0, "late_stale": 0, "duplicate": 0, "out_of_order": 0, "invalid": 0}
    stale_bits = (1 << 10) | (1 << 11)
    for r in flags:
        f, n = int(r["flags"]), int(r["n"])
        names[f] = n
        if f & (1 << 1):
            counts["duplicate"] += n
        elif f & (1 << 2):
            counts["out_of_order"] += n
        elif f & stale_bits:
            counts["late_stale"] += n
        elif f & ~((1 << 0) | (1 << 12)) & ((1 << FLAG_COUNT) - 1):
            counts["invalid"] += n
        else:
            counts["valid"] += n
    lost = conn.execute("SELECT COALESCE(SUM(missing), 0) FROM gaps WHERE node_id = ? AND "
                        "detected_ms >= ?", (node_id, since_ms)).fetchone()[0]
    dup_delivery = conn.execute("SELECT COUNT(*) FROM ingest_log WHERE node_id = ? AND "
                                "status = 'duplicate_delivery' AND received_ms >= ?",
                                (node_id, since_ms)).fetchone()[0]
    rejected = conn.execute("SELECT COUNT(*) FROM ingest_log WHERE node_id = ? AND "
                            "status = 'rejected' AND received_ms >= ?",
                            (node_id, since_ms)).fetchone()[0]
    counts["lost"] = int(lost)
    counts["duplicate_delivery"] = int(dup_delivery)
    counts["malformed"] = int(rejected)
    return counts
