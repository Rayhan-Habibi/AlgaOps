"""In-process event bus feeding the dashboard's Server-Sent Events stream.

publish() is called from the pipeline worker thread; each SSE client owns a
bounded asyncio queue (oldest events are dropped for slow clients — the
dashboard re-reads state from the REST API after reconnecting).
"""

from __future__ import annotations

import asyncio
import itertools
import json
import threading
import time
from typing import Any, AsyncIterator

HEARTBEAT_S = 15.0


class EventBus:
    def __init__(self, max_queue: int = 500) -> None:
        self._lock = threading.Lock()
        self._subs: dict[int, tuple[asyncio.AbstractEventLoop, asyncio.Queue]] = {}
        self._ids = itertools.count(1)
        self._event_ids = itertools.count(1)
        self._max_queue = max_queue
        self.published = 0

    def publish(self, event: str, data: dict[str, Any]) -> None:
        msg = (next(self._event_ids), event, data)
        self.published += 1
        with self._lock:
            subs = list(self._subs.values())
        for loop, q in subs:
            try:
                loop.call_soon_threadsafe(self._put, q, msg)
            except RuntimeError:
                pass  # loop closed

    @staticmethod
    def _put(q: asyncio.Queue, msg) -> None:
        if q.full():
            try:
                q.get_nowait()
            except asyncio.QueueEmpty:
                pass
        q.put_nowait(msg)

    def subscribe(self) -> tuple[int, asyncio.Queue]:
        loop = asyncio.get_running_loop()
        q: asyncio.Queue = asyncio.Queue(maxsize=self._max_queue)
        sid = next(self._ids)
        with self._lock:
            self._subs[sid] = (loop, q)
        return sid, q

    def unsubscribe(self, sid: int) -> None:
        with self._lock:
            self._subs.pop(sid, None)

    @property
    def subscribers(self) -> int:
        with self._lock:
            return len(self._subs)

    async def stream(self, is_disconnected) -> AsyncIterator[str]:
        sid, q = self.subscribe()
        try:
            yield "retry: 3000\n\n"
            yield _format(0, "hello", {"ts_ms": int(time.time() * 1000)})
            while True:
                if await is_disconnected():
                    break
                try:
                    eid, event, data = await asyncio.wait_for(q.get(), timeout=HEARTBEAT_S)
                    yield _format(eid, event, data)
                except asyncio.TimeoutError:
                    yield _format(0, "heartbeat", {"ts_ms": int(time.time() * 1000)})
        finally:
            self.unsubscribe(sid)


def _format(eid: int, event: str, data: dict[str, Any]) -> str:
    body = json.dumps(data, separators=(",", ":"), ensure_ascii=False)
    head = f"id: {eid}\n" if eid else ""
    return f"{head}event: {event}\ndata: {body}\n\n"
