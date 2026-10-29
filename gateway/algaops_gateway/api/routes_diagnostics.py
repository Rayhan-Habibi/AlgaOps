"""Decision history and decision detail (traceable to window + audit event)."""

from __future__ import annotations

import json

from fastapi import APIRouter, Depends, HTTPException, Query

from .server import AppContext, get_ctx

router = APIRouter(tags=["diagnostics"])

OUTCOMES = ("NORMAL", "CHECK_SENSOR", "CHECK_EQUIPMENT", "NOT_EVALUATED")


@router.get("/diagnostics")
def list_diagnostics(ctx: AppContext = Depends(get_ctx), node_id: str | None = None,
                     outcome: str | None = None, kind: str | None = None,
                     since_ms: int | None = None, until_ms: int | None = None,
                     before_id: int | None = None,
                     limit: int = Query(100, ge=1, le=1000)) -> dict:
    clauses, args = [], []
    if node_id:
        clauses.append("node_id = ?")
        args.append(node_id)
    if outcome:
        if outcome not in OUTCOMES:
            raise HTTPException(400, f"outcome must be one of {OUTCOMES}")
        clauses.append("outcome = ?")
        args.append(outcome)
    if kind:
        clauses.append("kind = ?")
        args.append(kind)
    if since_ms is not None:
        clauses.append("decided_ms >= ?")
        args.append(since_ms)
    if until_ms is not None:
        clauses.append("decided_ms <= ?")
        args.append(until_ms)
    if before_id is not None:
        clauses.append("id < ?")
        args.append(before_id)
    where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
    rows = ctx.db.reader().execute(
        f"SELECT id, window_id, audit_seq, decision_json FROM diagnostics{where} "
        "ORDER BY id DESC LIMIT ?", (*args, limit)).fetchall()
    items = []
    for r in rows:
        d = json.loads(r["decision_json"])
        items.append({
            "id": r["id"], "window_id": r["window_id"], "audit_seq": r["audit_seq"],
            "node_id": d["node_id"], "kind": d["kind"], "outcome": d["outcome"],
            "trust_score": d["trust"]["score"], "trusted": d["trust"]["trusted"],
            "flags": d["trust"]["flags"], "mode": d["mode"], "method": d["method"],
            "seq": d["seq"], "boot_id": d["boot_id"], "timestamp_ms": d["timestamp_ms"],
            "received_ms": d["received_ms"], "decided_ms": d["decided_ms"],
            "data_source": d["data_source"], "alert_event": d["alert"]["event"],
            "reason": d["reason"]})
    next_before = items[-1]["id"] if len(items) == limit else None
    return {"items": items, "next_before_id": next_before}


@router.get("/diagnostics/{diag_id}")
def get_diagnostic(diag_id: int, ctx: AppContext = Depends(get_ctx)) -> dict:
    conn = ctx.db.reader()
    r = conn.execute("SELECT * FROM diagnostics WHERE id = ?", (diag_id,)).fetchone()
    if r is None:
        raise HTTPException(404, f"decision {diag_id} not found")
    out = {"id": r["id"], "decision": json.loads(r["decision_json"]),
           "decision_digest": r["decision_digest"], "latency_ms": r["latency_ms"],
           "audit_seq": r["audit_seq"], "window": None, "audit_event": None}
    if r["window_id"] is not None:
        w = conn.execute("SELECT id, record_json, record_digest, payload_digest FROM windows "
                         "WHERE id = ?", (r["window_id"],)).fetchone()
        if w:
            out["window"] = {"id": w["id"], "record": json.loads(w["record_json"]),
                             "record_digest": w["record_digest"],
                             "payload_digest": w["payload_digest"]}
    if r["audit_seq"] is not None:
        e = conn.execute("SELECT seq, ts_ms, type, prev_hash, hash FROM audit_events "
                         "WHERE seq = ?", (r["audit_seq"],)).fetchone()
        out["audit_event"] = dict(e) if e else None
    return out
