"""Gateway health/readiness, overall status and per-node summaries."""

from __future__ import annotations

import json
import time

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import JSONResponse

from .. import __version__
from ..ingest.freshness import classify_liveness, completeness
from ..storage import repositories as repo
from .server import AppContext, get_ctx

router = APIRouter(tags=["status"])

ACTIVE_WINDOW_MS = 10 * 60 * 1000


def _now() -> int:
    return time.time_ns() // 1_000_000


def data_label(sources: dict[str, int]) -> str:
    synthetic = sources.get("simulator", 0) + sources.get("replay", 0)
    hardware = sources.get("hardware", 0)
    if synthetic and hardware:
        return "MIXED: DEMO/SYNTHETIC + LIVE HARDWARE"
    if synthetic:
        return "DEMO / SYNTHETIC"
    if hardware:
        return "LIVE HARDWARE"
    return "NO DATA"


@router.get("/health")
def health() -> dict:
    return {"status": "ok"}


@router.get("/ready")
def ready(ctx: AppContext = Depends(get_ctx)):
    problems = []
    try:
        ctx.db.reader().execute("SELECT 1").fetchone()
    except Exception as exc:  # pragma: no cover - depends on disk state
        problems.append(f"database: {exc}")
    t = getattr(ctx.pipeline, "_thread", None)
    if t is None or not t.is_alive():
        problems.append("pipeline worker not running")
    if ctx.mqtt is not None and ctx.mqtt.state != "connected":
        problems.append(f"mqtt {ctx.mqtt.state}")
    body = {"ready": not problems, "problems": problems}
    return JSONResponse(body, status_code=200 if not problems else 503)


def _sources(conn, now: int) -> dict[str, int]:
    out: dict[str, int] = {}
    rows = conn.execute("SELECT data_source, COUNT(*) FROM nodes WHERE last_seen_ms >= ? "
                        "GROUP BY data_source", (now - ACTIVE_WINDOW_MS,)).fetchall()
    if not rows:
        rows = conn.execute("SELECT data_source, COUNT(*) FROM nodes GROUP BY data_source"
                            ).fetchall()
    for r in rows:
        out[r[0]] = int(r[1])
    return out


@router.get("/status")
def status(ctx: AppContext = Depends(get_ctx)) -> dict:
    conn = ctx.db.reader()
    now = _now()
    seq, head = ctx.pipeline.chain.head
    cp = conn.execute("SELECT seq, hash, ts_ms FROM checkpoints ORDER BY seq DESC LIMIT 1"
                      ).fetchone()
    last_ver = conn.execute("SELECT payload FROM audit_events WHERE type = 'audit.verified' "
                            "ORDER BY seq DESC LIMIT 1").fetchone()
    sources = _sources(conn, now)
    mqtt = ctx.mqtt
    return {
        "gateway_id": ctx.config.gateway_id,
        "site": ctx.config.site,
        "version": __version__,
        "engine_version": ctx.lib.version,
        "policy": {"name": ctx.policy.meta.get("name"), "version": ctx.policy.meta.get("version"),
                   "hash": ctx.policy.hash},
        "started_ms": ctx.started_ms,
        "now_ms": now,
        "uptime_s": round((now - ctx.started_ms) / 1000.0, 1) if ctx.started_ms else None,
        "read_only": True,
        "mqtt": None if mqtt is None else {
            "state": mqtt.state, "host": mqtt.host, "port": mqtt.port, "connects": mqtt.connects,
            "disconnects": mqtt.disconnects, "last_error": mqtt.last_error},
        "pipeline": ctx.pipeline.stats.snapshot(),
        "audit": {
            "head_seq": seq, "head_hash": head,
            "last_checkpoint": dict(cp) if cp else None,
            "last_verification": json.loads(last_ver["payload"]) if last_ver else None,
        },
        "data": {"label": data_label(sources), "sources": sources},
        "nodes": conn.execute("SELECT COUNT(*) FROM nodes").fetchone()[0],
        "sse_clients": ctx.events.subscribers,
    }


def node_summary(ctx: AppContext, node: dict, now: int) -> dict:
    conn = ctx.db.reader()
    nid = node["node_id"]
    latest = repo.latest_decision(conn, nid)
    window_ms = None
    last_window = conn.execute("SELECT record_json FROM windows WHERE node_id = ? ORDER BY id "
                               "DESC LIMIT 1", (nid,)).fetchone()
    if last_window:
        window_ms = json.loads(last_window["record_json"]).get("window_ms")
    stale_after = int(ctx.policy.effective.get("window.stream_timeout_ms") or 15000)
    since = now - ACTIVE_WINDOW_MS
    received = conn.execute("SELECT COUNT(*) FROM windows WHERE node_id = ? AND received_ms >= ?",
                            (nid, since)).fetchone()[0]
    lost = conn.execute("SELECT COALESCE(SUM(missing), 0) FROM gaps WHERE node_id = ? AND "
                        "detected_ms >= ?", (nid, since)).fetchone()[0]
    alert = repo.current_open_alert(conn, nid)
    status = json.loads(node["status_json"]) if node.get("status_json") else None
    return {
        "node_id": nid,
        "data_source": node["data_source"],
        "fw_version": node.get("fw_version"),
        "last_boot_id": node.get("last_boot_id"),
        "first_seen_ms": node["first_seen_ms"],
        "last_seen_ms": node["last_seen_ms"],
        "liveness": classify_liveness(now, node["last_seen_ms"], window_ms, stale_after),
        "connection": status.get("state") if status else None,
        "status": status,
        "window_ms": window_ms,
        "completeness_10min": completeness(int(received), int(received) + int(lost)),
        "latest": latest,
        "open_alert": dict(alert) if alert else None,
        "baselines": [{"id": b["id"], "mode": b["mode"], "n": b["n"],
                       "activated_ms": b["activated_ms"]}
                      for b in repo.active_baselines(conn, nid)],
        "packets_1h": repo.packet_counts(conn, nid, now - 3600_000),
    }


@router.get("/nodes")
def nodes(ctx: AppContext = Depends(get_ctx)) -> dict:
    now = _now()
    return {"nodes": [node_summary(ctx, n, now) for n in repo.list_nodes(ctx.db.reader())],
            "now_ms": now}


@router.get("/nodes/{node_id}")
def node(node_id: str, ctx: AppContext = Depends(get_ctx)) -> dict:
    row = ctx.db.reader().execute("SELECT * FROM nodes WHERE node_id = ?", (node_id,)).fetchone()
    if row is None:
        raise HTTPException(404, f"node {node_id} not found")
    return node_summary(ctx, dict(row), _now())


@router.get("/pbr/context")
def pbr_context(ctx: AppContext = Depends(get_ctx)) -> dict:
    if ctx.pbr is None:
        return {"enabled": False, "note": "PBR process data is read only when explicitly enabled "
                                          "and approved; it is never used for diagnosis."}
    return {"enabled": True, **ctx.pbr.snapshot()}
