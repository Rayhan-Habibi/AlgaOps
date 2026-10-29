"""Alert history/detail and technician feedback (true / false alarm)."""

from __future__ import annotations

import json

from fastapi import APIRouter, Depends, HTTPException, Query
from pydantic import BaseModel, Field

from ..diagnostics.feedback import FeedbackError
from ..storage import repositories as repo
from .server import AppContext, get_ctx, require_token

router = APIRouter(tags=["alerts"])


class FeedbackIn(BaseModel):
    verdict: str = Field(pattern="^(true_alarm|false_alarm|unsure)$")
    note: str = Field(default="", max_length=500)
    technician: str = Field(min_length=1, max_length=64)


@router.get("/alerts")
def list_alerts(ctx: AppContext = Depends(get_ctx), node_id: str | None = None,
                status: str | None = None, limit: int = Query(100, ge=1, le=1000)) -> dict:
    clauses, args = [], []
    if node_id:
        clauses.append("a.node_id = ?")
        args.append(node_id)
    if status:
        clauses.append("a.status = ?")
        args.append(status)
    where = (" WHERE " + " AND ".join(clauses)) if clauses else ""
    rows = repo.rows(ctx.db.reader(),
                     "SELECT a.*, (SELECT verdict FROM feedback f WHERE f.alert_id = a.id "
                     "ORDER BY f.id DESC LIMIT 1) AS last_verdict FROM alerts a"
                     f"{where} ORDER BY a.id DESC LIMIT ?", (*args, limit))
    return {"items": rows}


@router.get("/alerts/{alert_id}")
def get_alert(alert_id: int, ctx: AppContext = Depends(get_ctx)) -> dict:
    conn = ctx.db.reader()
    a = conn.execute("SELECT * FROM alerts WHERE id = ?", (alert_id,)).fetchone()
    if a is None:
        raise HTTPException(404, f"alert {alert_id} not found")
    alert = dict(a)
    end = alert["closed_ms"] or 2**62
    decisions = [json.loads(r["decision_json"]) | {"id": r["id"]} for r in conn.execute(
        "SELECT id, decision_json FROM diagnostics WHERE node_id = ? AND decided_ms BETWEEN ? "
        "AND ? AND outcome = ? ORDER BY id LIMIT 200",
        (alert["node_id"], alert["opened_ms"], end, alert["kind"]))]
    feedback = repo.rows(conn, "SELECT * FROM feedback WHERE alert_id = ? ORDER BY id",
                         (alert_id,))
    return {"alert": alert, "decisions": decisions, "feedback": feedback}


@router.post("/alerts/{alert_id}/feedback", dependencies=[Depends(require_token)])
def post_feedback(alert_id: int, body: FeedbackIn, ctx: AppContext = Depends(get_ctx)) -> dict:
    try:
        return ctx.pipeline.record_feedback(alert_id, body.verdict, body.note, body.technician)
    except FeedbackError as exc:
        raise HTTPException(400, str(exc)) from None
