"""FastAPI application factory for the local dashboard API.

Security posture: bound to 127.0.0.1 by default; strict same-origin Content
Security Policy; no CORS; state-changing endpoints require the API token
whenever one is configured.  There is no endpoint that controls the pump,
blower or PBR — the API is read-only towards the plant.
"""

from __future__ import annotations

import hmac
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

from ..config import GatewayConfig
from ..core.engine import Policy
from ..core.libalgaops import CoreLibrary
from ..storage.database import Database
from .events import EventBus

SECURITY_HEADERS = {
    "Content-Security-Policy": ("default-src 'self'; script-src 'self'; style-src 'self'; "
                                "img-src 'self' data:; connect-src 'self'; font-src 'self'; "
                                "object-src 'none'; base-uri 'none'; frame-ancestors 'none'; "
                                "form-action 'self'"),
    "X-Content-Type-Options": "nosniff",
    "X-Frame-Options": "DENY",
    "Referrer-Policy": "no-referrer",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Resource-Policy": "same-origin",
    "Permissions-Policy": "camera=(), microphone=(), geolocation=()",
}


@dataclass
class AppContext:
    config: GatewayConfig
    lib: CoreLibrary
    policy: Policy
    db: Database
    pipeline: Any
    events: EventBus
    mqtt: Any = None
    token: str | None = None
    started_ms: int = 0
    dashboard_dir: Path | None = None
    replay: Any = None
    evaluation: Any = None
    pbr: Any = None
    extra: dict[str, Any] = field(default_factory=dict)


def get_ctx(request: Request) -> AppContext:
    return request.app.state.ctx


def require_token(request: Request, ctx: AppContext = Depends(get_ctx)) -> None:
    """Guard for state-changing endpoints."""
    if ctx.token is None:
        return
    supplied = request.headers.get("x-algaops-token", "")
    auth = request.headers.get("authorization", "")
    if auth.lower().startswith("bearer "):
        supplied = auth[7:].strip()
    if not supplied or not hmac.compare_digest(supplied.encode(), ctx.token.encode()):
        raise HTTPException(status_code=401, detail="missing or invalid API token")


def create_app(ctx: AppContext) -> FastAPI:
    app = FastAPI(title="AlgaOps Assurance gateway", version="2.0.0", docs_url="/api/docs",
                  redoc_url=None, openapi_url="/api/openapi.json")
    app.state.ctx = ctx

    @app.middleware("http")
    async def security_headers(request: Request, call_next):
        response = await call_next(request)
        for k, v in SECURITY_HEADERS.items():
            if request.url.path.startswith("/api/docs") and k == "Content-Security-Policy":
                continue  # interactive API docs need inline scripts
            response.headers.setdefault(k, v)
        if request.url.path.startswith("/api/"):
            response.headers.setdefault("Cache-Control", "no-store")
        return response

    @app.exception_handler(KeyError)
    async def not_found(request: Request, exc: KeyError):
        return JSONResponse(status_code=404, content={"detail": str(exc).strip("'\"")})

    @app.exception_handler(ValueError)
    async def bad_request(request: Request, exc: ValueError):
        return JSONResponse(status_code=400, content={"detail": str(exc)})

    from . import (
        routes_analytics,
        routes_audit,
        routes_baselines,
        routes_diagnostics,
        routes_export,
        routes_feedback,
        routes_history,
        routes_replay,
        routes_status,
    )
    from .static import mount_dashboard

    for module in (routes_status, routes_diagnostics, routes_history, routes_feedback,
                   routes_audit, routes_baselines, routes_export, routes_replay,
                   routes_analytics):
        app.include_router(module.router, prefix="/api")

    from fastapi.responses import StreamingResponse

    @app.get("/api/events", include_in_schema=False)
    async def sse(request: Request):
        return StreamingResponse(ctx.events.stream(request.is_disconnected),
                                 media_type="text/event-stream",
                                 headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"})

    mount_dashboard(app, ctx.dashboard_dir)
    return app
