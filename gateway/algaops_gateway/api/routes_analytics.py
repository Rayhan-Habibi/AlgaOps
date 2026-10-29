"""Analytics: method comparison on live data, labelled synthetic evaluation."""

from __future__ import annotations

import json
import logging
import threading
from pathlib import Path

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel

from ..analytics.compare_methods import compare_methods
from .server import AppContext, get_ctx, require_token

log = logging.getLogger(__name__)
router = APIRouter(tags=["analytics"])


class EvaluationManager:
    def __init__(self, ctx: AppContext) -> None:
        self.ctx = ctx
        self.state = "idle"
        self.error: str | None = None
        self._lock = threading.Lock()

    @property
    def path(self) -> Path:
        return self.ctx.config.resolve("var/evaluation/latest.json")

    def latest(self) -> dict | None:
        p = self.path
        return json.loads(p.read_text(encoding="utf-8")) if p.is_file() else None

    def start(self, full: bool) -> None:
        with self._lock:
            if self.state == "running":
                raise HTTPException(409, "an evaluation is already running")
            self.state, self.error = "running", None

        def run() -> None:
            try:
                from ..analytics.evaluate import FULL, QUICK, evaluate
                rep = evaluate(self.ctx.lib, self.ctx.policy, FULL if full else QUICK)
                self.path.parent.mkdir(parents=True, exist_ok=True)
                tmp = self.path.with_suffix(".part")
                tmp.write_text(json.dumps(rep, indent=1), encoding="utf-8")
                tmp.replace(self.path)
                self.state = "done"
            except Exception as exc:
                log.exception("evaluation failed")
                self.state, self.error = "failed", str(exc)
        threading.Thread(target=run, name="evaluation", daemon=True).start()


class EvaluationIn(BaseModel):
    full: bool = False


def _mgr(ctx: AppContext) -> EvaluationManager:
    if ctx.evaluation is None:
        ctx.evaluation = EvaluationManager(ctx)
    return ctx.evaluation


@router.get("/analytics/methods")
def methods(ctx: AppContext = Depends(get_ctx), node_id: str | None = None,
            since_ms: int | None = None, until_ms: int | None = None) -> dict:
    return compare_methods(ctx.db.reader(), node_id, since_ms, until_ms)


@router.get("/analytics/evaluation")
def evaluation(ctx: AppContext = Depends(get_ctx)) -> dict:
    m = _mgr(ctx)
    return {"state": m.state, "error": m.error, "report": m.latest()}


@router.post("/analytics/evaluation", dependencies=[Depends(require_token)])
def run_evaluation(body: EvaluationIn, ctx: AppContext = Depends(get_ctx)) -> dict:
    try:
        import algaops_sim  # noqa: F401
    except ImportError:
        raise HTTPException(501, "simulator package not installed") from None
    _mgr(ctx).start(body.full)
    return {"state": "running", "full": body.full}
