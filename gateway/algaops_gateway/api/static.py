"""Serve the built dashboard (dashboard/dist) from the gateway — no CDN, no
external requests; everything the browser needs is local."""

from __future__ import annotations

from pathlib import Path

from fastapi import FastAPI
from fastapi.responses import FileResponse, HTMLResponse

FALLBACK = """<!doctype html><html lang="en"><meta charset="utf-8">
<title>AlgaOps gateway</title><body style="font-family:sans-serif;max-width:40rem;margin:3rem auto">
<h1>AlgaOps gateway is running</h1>
<p>The dashboard has not been built yet. Build it once (Node.js needed only at build time):</p>
<pre>cd dashboard
npm ci
npm run build</pre>
<p>The JSON API is available under <code>/api/</code> (see <code>/api/docs</code>).</p></body></html>"""

CACHE = {".js": "public, max-age=31536000, immutable", ".css": "public, max-age=31536000, immutable"}


def mount_dashboard(app: FastAPI, dist: Path | None) -> None:
    root = dist.resolve() if dist else None
    index = root / "index.html" if root else None

    @app.get("/{path:path}", include_in_schema=False)
    async def dashboard(path: str):
        if path.startswith("api/"):
            return HTMLResponse("not found", status_code=404)
        if root is None or index is None or not index.is_file():
            return HTMLResponse(FALLBACK)
        target = (root / path).resolve() if path else index
        try:
            target.relative_to(root)
        except ValueError:
            return HTMLResponse("not found", status_code=404)
        if target.is_file():
            headers = {}
            cc = CACHE.get(target.suffix)
            if cc and "/assets/" in target.as_posix():
                headers["Cache-Control"] = cc
            elif target.name in ("index.html", "sw.js", "manifest.webmanifest"):
                headers["Cache-Control"] = "no-cache"
            media = "application/manifest+json" if target.suffix == ".webmanifest" else None
            return FileResponse(target, headers=headers, media_type=media)
        return FileResponse(index, headers={"Cache-Control": "no-cache"})  # SPA route
