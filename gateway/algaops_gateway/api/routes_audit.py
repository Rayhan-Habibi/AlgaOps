"""Audit chain status, on-demand verification and event listing."""

from __future__ import annotations

import json

from fastapi import APIRouter, Depends, Query

from ..audit.checkpoint import read_db_checkpoints
from ..audit.verify import verify_database
from .server import AppContext, get_ctx, require_token

router = APIRouter(tags=["audit"])


def run_verification(ctx: AppContext) -> dict:
    """Verify from a fresh read-only snapshot, then audit the verification run."""
    rconn = ctx.db.connect_readonly()
    try:
        rconn.execute("BEGIN")
        report = verify_database(rconn, ctx.pipeline.hasher,
                                 ctx.config.resolve(ctx.config.storage.checkpoint_file))
        rconn.execute("COMMIT")
    finally:
        rconn.close()
    result = report.to_dict()
    seq = ctx.pipeline.record_audit("audit.verified", None, {
        "ok": report.ok, "head_seq": report.head_seq, "head_hash": report.head_hash,
        "issues": len(report.issues), "events": report.events})
    result["recorded_as_seq"] = seq
    return result


@router.get("/audit/status")
def audit_status(ctx: AppContext = Depends(get_ctx)) -> dict:
    conn = ctx.db.reader()
    seq, head = ctx.pipeline.chain.head
    last = conn.execute("SELECT seq, ts_ms, payload FROM audit_events WHERE type = "
                        "'audit.verified' ORDER BY seq DESC LIMIT 1").fetchone()
    counts = {r[0]: r[1] for r in conn.execute(
        "SELECT type, COUNT(*) FROM audit_events GROUP BY type")}
    cps = read_db_checkpoints(conn)
    return {
        "head_seq": seq, "head_hash": head, "event_counts": counts,
        "checkpoints": [{"seq": c.seq, "hash": c.hash, "ts_ms": c.ts_ms} for c in cps[-20:]],
        "checkpoint_file": str(ctx.config.resolve(ctx.config.storage.checkpoint_file)),
        "last_verification": (json.loads(last["payload"]) | {"ts_ms": last["ts_ms"]})
        if last else None,
        "hash_algorithm": "Ascon-Hash256 (NIST SP 800-232)",
    }


@router.post("/audit/verify", dependencies=[Depends(require_token)])
def audit_verify(ctx: AppContext = Depends(get_ctx)) -> dict:
    return run_verification(ctx)


@router.get("/audit/events")
def audit_events(ctx: AppContext = Depends(get_ctx), after_seq: int = 0,
                 type: str | None = None, limit: int = Query(100, ge=1, le=1000)) -> dict:
    sql = ("SELECT seq, ts_ms, type, node_id, payload, prev_hash, hash FROM audit_events "
           "WHERE seq > ?")
    args: list = [after_seq]
    if type:
        sql += " AND type = ?"
        args.append(type)
    sql += " ORDER BY seq LIMIT ?"
    args.append(limit)
    items = []
    for r in ctx.db.reader().execute(sql, args):
        d = dict(r)
        d["payload"] = json.loads(d["payload"])
        items.append(d)
    return {"items": items}
