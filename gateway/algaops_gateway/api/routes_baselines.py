"""Baseline management: list, capture from live trusted data, import, activate, retire."""

from __future__ import annotations

import json
import os
import tempfile

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from ..storage import repositories as repo
from .server import AppContext, get_ctx, require_token

router = APIRouter(tags=["baselines"])


class CaptureIn(BaseModel):
    node_id: str = Field(min_length=1, max_length=64)
    mode: str = Field(pattern="^(OFF|STARTUP|STEADY)$")
    windows: int = Field(ge=2, le=100000)
    activate: bool = False


class ImportIn(BaseModel):
    node_id: str = Field(min_length=1, max_length=64)
    csv: str = Field(min_length=10, max_length=2_000_000)
    activate: bool = False


@router.get("/baselines")
def list_baselines(ctx: AppContext = Depends(get_ctx), node_id: str | None = None) -> dict:
    sql = "SELECT * FROM baselines"
    args: tuple = ()
    if node_id:
        sql += " WHERE node_id = ?"
        args = (node_id,)
    items = []
    for b in repo.rows(ctx.db.reader(), sql + " ORDER BY id DESC", args):
        b["stats"] = json.loads(b.pop("stats_json"))
        b["source"] = json.loads(b.pop("source_json"))
        items.append(b)
    return {"items": items, "capture": ctx.pipeline.capture_status(),
            "min_samples": ctx.pipeline.min_baseline_samples}


@router.post("/baselines/capture", dependencies=[Depends(require_token)])
def start_capture(body: CaptureIn, ctx: AppContext = Depends(get_ctx)) -> dict:
    return ctx.pipeline.start_capture(body.node_id, body.mode, body.windows, body.activate)


@router.get("/baselines/capture")
def capture_status(ctx: AppContext = Depends(get_ctx)) -> dict:
    return {"sessions": ctx.pipeline.capture_status()}


@router.delete("/baselines/capture/{node_id}", dependencies=[Depends(require_token)])
def cancel_capture(node_id: str, ctx: AppContext = Depends(get_ctx)) -> dict:
    if not ctx.pipeline.cancel_capture(node_id):
        raise HTTPException(404, f"no capture running for {node_id}")
    return {"cancelled": node_id}


@router.post("/baselines/import", dependencies=[Depends(require_token)])
def import_csv(body: ImportIn, ctx: AppContext = Depends(get_ctx)) -> dict:
    fd, path = tempfile.mkstemp(suffix=".csv", prefix="algaops_baseline_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(body.csv)
        ids = ctx.pipeline.import_baseline_csv(body.node_id, path, body.activate)
    finally:
        os.unlink(path)
    if not ids:
        raise HTTPException(400, "no valid baseline could be built from the CSV")
    return {"baseline_ids": ids}


@router.post("/baselines/{baseline_id}/activate", dependencies=[Depends(require_token)])
def activate(baseline_id: int, ctx: AppContext = Depends(get_ctx)) -> dict:
    return ctx.pipeline.activate_baseline(baseline_id)


@router.post("/baselines/{baseline_id}/retire", dependencies=[Depends(require_token)])
def retire(baseline_id: int, ctx: AppContext = Depends(get_ctx)) -> dict:
    return ctx.pipeline.retire_baseline(baseline_id)
