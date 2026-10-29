"""Scenario replay for demos.

offline: the scenario runs through an isolated pipeline (temporary database,
         simulated clock) — results never mix with the live history.
live:    the scenario is published to the local MQTT broker in real time and
         flows through the normal live path (labelled DEMO / SYNTHETIC).
"""

from __future__ import annotations

import itertools
import logging
import threading
import time
from typing import Any

from fastapi import APIRouter, Depends, HTTPException
from pydantic import BaseModel, Field

from .server import AppContext, get_ctx, require_token

log = logging.getLogger(__name__)
router = APIRouter(tags=["replay"])


class ReplayManager:
    def __init__(self, ctx: AppContext) -> None:
        self.ctx = ctx
        self.jobs: dict[int, dict[str, Any]] = {}
        self._ids = itertools.count(1)
        self._lock = threading.Lock()
        self._baselines = None

    def scenarios(self) -> list[dict[str, Any]]:
        from algaops_sim.scenario import list_scenarios
        return [{"name": s.name, "description": s.description.strip(),
                 "windows": s.duration_windows,
                 "duration_s": round(s.duration_windows * s.window_ms / 1000.0, 1),
                 "node_id": s.node_id} for s in list_scenarios() if s.name != "baseline_modes"]

    def _job(self, kind: str, scenario: str) -> int:
        jid = next(self._ids)
        with self._lock:
            self.jobs[jid] = {"id": jid, "kind": kind, "scenario": scenario, "state": "running",
                              "started_ms": time.time_ns() // 1_000_000, "progress": 0.0}
            # keep the 20 most recent jobs
            for old in sorted(self.jobs)[:-20]:
                self.jobs.pop(old, None)
        return jid

    def _update(self, jid: int, **kw) -> None:
        with self._lock:
            if jid in self.jobs:
                self.jobs[jid].update(kw)

    def start_offline(self, scenario: str) -> int:
        jid = self._job("offline", scenario)

        def run() -> None:
            try:
                from ..analytics.evaluate import build_baselines, run_scenario
                if self._baselines is None:
                    self._baselines = build_baselines(self.ctx.lib, self.ctx.policy,
                                                      "baseline_modes")
                result = run_scenario(self.ctx.lib, self.ctx.policy, scenario, self._baselines,
                                      keep_timeline=True)
                self._update(jid, state="done", progress=1.0, result=result)
            except Exception as exc:
                log.exception("offline replay failed")
                self._update(jid, state="failed", error=str(exc))
        threading.Thread(target=run, name=f"replay-{jid}", daemon=True).start()
        return jid

    def start_live(self, scenario: str) -> int:
        if self.ctx.mqtt is None:
            raise HTTPException(409, "live replay needs the MQTT ingest enabled")
        jid = self._job("live", scenario)
        stop = threading.Event()
        self._update(jid, stop=stop)

        def run() -> None:
            try:
                from algaops_sim.mqtt_publisher import Publisher, offline_status, publish_schedule
                from algaops_sim.replay import generate_stream
                from algaops_sim.scenario import load_scenario
                sc = load_scenario(scenario)
                events, _ = generate_stream(sc, live=True,
                                            topic_prefix=self.ctx.config.mqtt.topic_prefix)
                items = [(e.t_ms, e.topic, e.payload, e.kind == "status") for e in events]
                lwt_topic, lwt_payload = offline_status(sc.node_id,
                                                        self.ctx.config.mqtt.topic_prefix)
                cfg = self.ctx.config.mqtt
                with Publisher(cfg.host, cfg.port, client_id=f"algaops-replay-{jid}",
                               lwt_topic=lwt_topic, lwt_payload=lwt_payload) as pub:
                    sent = publish_schedule(
                        items, pub, stop,
                        lambda n, total: self._update(jid, progress=round(n / total, 3),
                                                      sent=n, total=total))
                    pub.publish(lwt_topic, lwt_payload, retain=True)
                self._update(jid, state="stopped" if stop.is_set() else "done", sent=sent)
            except Exception as exc:
                log.exception("live replay failed")
                self._update(jid, state="failed", error=str(exc))
        threading.Thread(target=run, name=f"live-replay-{jid}", daemon=True).start()
        return jid

    def stop(self, jid: int) -> None:
        with self._lock:
            job = self.jobs.get(jid)
        if job is None:
            raise KeyError(f"job {jid} not found")
        ev = job.get("stop")
        if ev is not None:
            ev.set()

    def get(self, jid: int) -> dict[str, Any]:
        with self._lock:
            job = self.jobs.get(jid)
            if job is None:
                raise KeyError(f"job {jid} not found")
            return {k: v for k, v in job.items() if k != "stop"}


class ReplayIn(BaseModel):
    scenario: str = Field(pattern=r"^[a-z0-9_]{1,64}$")
    mode: str = Field(default="offline", pattern="^(offline|live)$")


def _mgr(ctx: AppContext) -> ReplayManager:
    if ctx.replay is None:
        if not ctx.config.demo.enabled:
            raise HTTPException(403, "demo replay is disabled in the gateway configuration")
        ctx.replay = ReplayManager(ctx)
    return ctx.replay


@router.get("/replay/scenarios")
def scenarios(ctx: AppContext = Depends(get_ctx)) -> dict:
    try:
        return {"items": _mgr(ctx).scenarios()}
    except ImportError:
        raise HTTPException(501, "simulator package not installed") from None


@router.post("/replay/run", dependencies=[Depends(require_token)])
def run(body: ReplayIn, ctx: AppContext = Depends(get_ctx)) -> dict:
    mgr = _mgr(ctx)
    names = {s["name"] for s in mgr.scenarios()}
    if body.scenario not in names:
        raise HTTPException(404, f"unknown scenario {body.scenario}")
    jid = mgr.start_offline(body.scenario) if body.mode == "offline" else \
        mgr.start_live(body.scenario)
    return {"job_id": jid, "mode": body.mode}


@router.get("/replay/jobs/{job_id}")
def job(job_id: int, ctx: AppContext = Depends(get_ctx)) -> dict:
    return _mgr(ctx).get(job_id)


@router.post("/replay/jobs/{job_id}/stop", dependencies=[Depends(require_token)])
def stop(job_id: int, ctx: AppContext = Depends(get_ctx)) -> dict:
    _mgr(ctx).stop(job_id)
    return {"stopping": job_id}
