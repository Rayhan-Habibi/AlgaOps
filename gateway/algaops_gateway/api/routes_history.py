"""Telemetry history: raw windows, down-sampled trends with mode bands, packet status."""

from __future__ import annotations

import json
import time

from fastapi import APIRouter, Depends, HTTPException, Query

from ..storage import repositories as repo
from .server import AppContext, get_ctx

router = APIRouter(tags=["history"])

METRICS = {"vib_rms_g": "w.vib_rms_g", "temp_c": "w.temp_c", "current_a": "w.current_a",
           "trust_score": "d.trust_score"}


@router.get("/history/windows")
def windows(node_id: str, ctx: AppContext = Depends(get_ctx), since_ms: int | None = None,
            until_ms: int | None = None, limit: int = Query(500, ge=1, le=5000)) -> dict:
    clauses, args = ["node_id = ?"], [node_id]
    if since_ms is not None:
        clauses.append("received_ms >= ?")
        args.append(since_ms)
    if until_ms is not None:
        clauses.append("received_ms <= ?")
        args.append(until_ms)
    rows = ctx.db.reader().execute(
        f"SELECT id, record_json FROM windows WHERE {' AND '.join(clauses)} "
        "ORDER BY id DESC LIMIT ?", (*args, limit)).fetchall()
    return {"items": [{"id": r["id"], **json.loads(r["record_json"])} for r in reversed(rows)]}


@router.get("/history/trend")
def trend(node_id: str, ctx: AppContext = Depends(get_ctx), metric: str = "vib_rms_g",
          since_ms: int | None = None, until_ms: int | None = None,
          max_points: int = Query(400, ge=10, le=5000)) -> dict:
    if metric not in METRICS:
        raise HTTPException(400, f"metric must be one of {tuple(METRICS)}")
    now = time.time_ns() // 1_000_000
    until = until_ms if until_ms is not None else now
    since = since_ms if since_ms is not None else until - 3600_000
    if since >= until:
        raise HTTPException(400, "since_ms must be before until_ms")
    col = METRICS[metric]
    rows = ctx.db.reader().execute(
        f"SELECT w.received_ms AS t, {col} AS v, d.outcome AS outcome, d.mode AS mode "
        "FROM windows w JOIN diagnostics d ON d.window_id = w.id "
        "WHERE w.node_id = ? AND w.received_ms BETWEEN ? AND ? ORDER BY w.id",
        (node_id, since, until)).fetchall()
    bucket = max(1, (until - since) // max_points)
    points: list[dict] = []
    modes: list[dict] = []
    markers: list[dict] = []
    cur = None
    for r in rows:
        t, v = int(r["t"]), r["v"]
        b = since + ((t - since) // bucket) * bucket
        if cur is None or cur["t"] != b:
            cur = {"t": b, "min": None, "max": None, "sum": 0.0, "n": 0}
            points.append(cur)
        if v is not None:
            cur["min"] = v if cur["min"] is None else min(cur["min"], v)
            cur["max"] = v if cur["max"] is None else max(cur["max"], v)
            cur["sum"] += v
            cur["n"] += 1
        m = r["mode"]
        if m != "UNKNOWN":
            if modes and modes[-1]["mode"] == m:
                modes[-1]["until"] = t
            else:
                modes.append({"mode": m, "since": t, "until": t})
        if r["outcome"] in ("CHECK_SENSOR", "CHECK_EQUIPMENT"):
            if not markers or markers[-1]["outcome"] != r["outcome"] or \
                    t - markers[-1]["until"] > bucket:
                markers.append({"outcome": r["outcome"], "since": t, "until": t})
            else:
                markers[-1]["until"] = t
    out = [{"t": p["t"], "min": p["min"], "max": p["max"],
            "mean": (p["sum"] / p["n"]) if p["n"] else None, "n": p["n"]} for p in points]
    return {"node_id": node_id, "metric": metric, "since_ms": since, "until_ms": until,
            "bucket_ms": bucket, "points": out, "modes": modes, "markers": markers}


@router.get("/history/packets")
def packets(node_id: str, ctx: AppContext = Depends(get_ctx),
            since_ms: int | None = None) -> dict:
    now = time.time_ns() // 1_000_000
    since = since_ms if since_ms is not None else now - 3600_000
    conn = ctx.db.reader()
    counts = repo.packet_counts(conn, node_id, since)
    gaps = repo.rows(conn, "SELECT boot_id, from_seq, to_seq, missing, detected_ms FROM gaps "
                     "WHERE node_id = ? AND detected_ms >= ? ORDER BY id DESC LIMIT 200",
                     (node_id, since))
    seqs = repo.rows(conn, "SELECT w.seq, w.boot_id, w.received_ms, d.flags, d.is_trusted "
                     "FROM windows w JOIN diagnostics d ON d.window_id = w.id WHERE "
                     "w.node_id = ? AND w.received_ms >= ? ORDER BY w.id DESC LIMIT 600",
                     (node_id, since))
    received = sum(v for k, v in counts.items() if k in ("valid", "late_stale", "duplicate",
                                                          "out_of_order", "invalid"))
    expected = received + counts["lost"]
    return {"node_id": node_id, "since_ms": since, "counts": counts,
            "completeness": (received / expected) if expected else None,
            "gaps": gaps, "recent": list(reversed(seqs))}
