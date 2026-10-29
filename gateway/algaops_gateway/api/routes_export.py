"""Exports: verifiable evidence bundles and CSV/JSONL history downloads."""

from __future__ import annotations

import re
import time
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import FileResponse, StreamingResponse
from pydantic import BaseModel, Field

from ..export.csv_export import TABLES, export_csv
from ..export.evidence_package import ExportScope, build_bundle
from ..export.json_export import export_decisions_jsonl
from .server import AppContext, get_ctx, require_token

router = APIRouter(tags=["export"])
SAFE_NAME = re.compile(r"^algaops-evidence-[0-9A-Za-z_.-]+\.zip$")


class EvidenceIn(BaseModel):
    node_id: str | None = Field(default=None, max_length=64)
    since_ms: int | None = None
    until_ms: int | None = None


def export_dir(ctx: AppContext) -> Path:
    d = ctx.config.resolve(ctx.config.storage.export_dir)
    d.mkdir(parents=True, exist_ok=True)
    return d


def create_evidence(ctx: AppContext, scope: ExportScope) -> dict:
    now = time.time_ns() // 1_000_000
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(now / 1000))
    name = f"algaops-evidence-{ctx.config.gateway_id}-{stamp}-{now % 1000:03d}.zip"
    path = export_dir(ctx) / name
    ctx.pipeline.checkpoint_now()   # the exported head is anchored in the external file too
    conn = ctx.db.connect_readonly()
    try:
        manifest = build_bundle(conn, ctx.pipeline.hasher, path, gateway_id=ctx.config.gateway_id,
                                engine_version=ctx.lib.version, policy_hash=ctx.policy.hash,
                                created_ms=now, scope=scope,
                                checkpoint_file=ctx.config.resolve(
                                    ctx.config.storage.checkpoint_file))
    finally:
        conn.close()
    seq = ctx.pipeline.record_audit("export.created", scope.node_id, {
        "file": name, "bundle_digest": manifest["bundle_digest"],
        "head_seq": manifest["chain"]["head_seq"], "head_hash": manifest["chain"]["head_hash"],
        "scope": manifest["scope"]})
    return {"file": name, "url": f"/api/export/files/{name}", "manifest": manifest,
            "recorded_as_seq": seq}


@router.post("/export/evidence", dependencies=[Depends(require_token)])
def evidence(body: EvidenceIn, ctx: AppContext = Depends(get_ctx)) -> dict:
    return create_evidence(ctx, ExportScope(body.node_id, body.since_ms, body.until_ms))


@router.get("/export/files")
def list_files(ctx: AppContext = Depends(get_ctx)) -> dict:
    d = export_dir(ctx)
    items = sorted(({"file": p.name, "bytes": p.stat().st_size,
                     "modified_ms": int(p.stat().st_mtime * 1000)}
                    for p in d.glob("algaops-evidence-*.zip")), key=lambda x: x["file"],
                   reverse=True)
    return {"items": items}


@router.get("/export/files/{name}")
def download(name: str, ctx: AppContext = Depends(get_ctx)):
    if not SAFE_NAME.match(name):
        raise HTTPException(400, "invalid file name")
    p = export_dir(ctx) / name
    if not p.is_file():
        raise HTTPException(404, "file not found")
    return FileResponse(p, media_type="application/zip", filename=name)


@router.get("/export/csv")
def csv_download(table: str, ctx: AppContext = Depends(get_ctx), node_id: str | None = None,
                 since_ms: int | None = None, until_ms: int | None = None):
    if table not in TABLES:
        raise HTTPException(400, f"table must be one of {tuple(TABLES)}")
    conn = ctx.db.connect_readonly()

    def gen():
        try:
            yield from export_csv(conn, table, node_id, since_ms, until_ms)
        finally:
            conn.close()
    return StreamingResponse(gen(), media_type="text/csv", headers={
        "Content-Disposition": f'attachment; filename="algaops-{table}.csv"'})


@router.get("/export/decisions.jsonl")
def jsonl_download(ctx: AppContext = Depends(get_ctx), node_id: str | None = None,
                   since_ms: int | None = None, until_ms: int | None = None):
    conn = ctx.db.connect_readonly()

    def gen():
        try:
            yield from export_decisions_jsonl(conn, node_id, since_ms, until_ms)
        finally:
            conn.close()
    return StreamingResponse(gen(), media_type="application/x-ndjson", headers={
        "Content-Disposition": 'attachment; filename="algaops-decisions.jsonl"'})
