"""Helpers shared by gateway tests (importable as `gwtest`)."""

from __future__ import annotations

import asyncio
import copy
import json
import socket
import threading
import time

T0 = 1_767_225_600_000  # 2026-01-01T00:00:00Z
TOPIC = "algaops/v1/nodes/node-a/telemetry"


class Clock:
    def __init__(self, t: int = T0) -> None:
        self.t = t

    def __call__(self) -> int:
        return self.t


BASE_DOC = {
    "schema": "algaops.telemetry_window.v1",
    "node_id": "node-a",
    "boot_id": 7,
    "seq": 1,
    "t_start_ms": T0 - 1280,
    "t_end_ms": T0,
    "window_ms": 1280,
    "time_synced": True,
    "sample_rate_hz": 800,
    "samples_expected": 1024,
    "samples_valid": 1024,
    "vib": {"rms_g": 0.42, "rms_axis_g": [0.2425, 0.2425, 0.2425],
            "p2p_g": [1.4, 1.3, 1.5]},
    "temp_c": 31.0,
    "current_a": 1.8,
    "self_test": "pass",
    "faults": [],
    "fw": "test",
    "source": "simulator",
}


def _vib(v: float) -> dict:
    a = round(v / 3 ** 0.5, 6)
    return {"rms_g": round(v, 6), "rms_axis_g": [a, a, a], "p2p_g": [a * 5, a * 5, a * 5]}


def make_doc(seq: int = 1, **over) -> dict:
    """A healthy window; values vary deterministically with seq (a live sensor is
    never bit-identical from window to window)."""
    d = copy.deepcopy(BASE_DOC)
    d["seq"] = seq
    d["t_end_ms"] = T0 + (seq - 1) * 1280
    d["t_start_ms"] = d["t_end_ms"] - 1280
    d["vib"] = _vib(0.42 + 0.002 * (((seq * 7) % 5) - 2))
    d["temp_c"] = 31.0 + 0.0625 * ((seq * 3) % 3)
    d["current_a"] = round(1.8 + 0.01 * ((seq % 3) - 1), 3)
    for k, v in over.items():
        if k == "vib_rms":
            d["vib"] = _vib(v) if v == v else {"rms_g": None, "rms_axis_g": [0.2, 0.2, 0.2],
                                               "p2p_g": [1.0, 1.0, 1.0]}
        else:
            d[k] = v
    return d


def payload(seq: int = 1, **over) -> bytes:
    return json.dumps(make_doc(seq, **over), separators=(",", ":")).encode()


def feed(pipe, clock: Clock, n: int, start_seq: int = 1, topic: str = TOPIC, **over) -> None:
    """Feed n healthy windows (1.28 s apart, received 50 ms after window end)."""
    for s in range(start_seq, start_seq + n):
        d = make_doc(s, **over)
        clock.t = d["t_end_ms"] + 50
        pipe.handle_message(topic, json.dumps(d).encode(), clock.t)


def last_decision(pipe) -> dict:
    r = pipe.db.writer.execute("SELECT id, decision_json FROM diagnostics ORDER BY id DESC "
                               "LIMIT 1").fetchone()
    return json.loads(r["decision_json"]) | {"id": r["id"]}


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class DevBroker:
    """amqtt broker in a background event loop (development/test only)."""

    def __init__(self, port: int, password_file: str | None = None) -> None:
        self.port = port
        self.password_file = password_file
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._broker = None

    def start(self) -> None:
        from amqtt.broker import Broker
        if not self._thread.is_alive():
            self._thread.start()
        if self.password_file:
            plugins = {"amqtt.plugins.authentication.FileAuthPlugin":
                       {"password_file": self.password_file}}
        else:
            plugins = {"amqtt.plugins.authentication.AnonymousAuthPlugin":
                       {"allow_anonymous": True}}
        cfg = {"listeners": {"default": {"type": "tcp", "bind": f"127.0.0.1:{self.port}"}},
               "plugins": plugins}

        async def go():
            self._broker = Broker(cfg)
            await self._broker.start()
        asyncio.run_coroutine_threadsafe(go(), self._loop).result(15)

    def shutdown_broker(self) -> None:
        if self._broker is not None:
            asyncio.run_coroutine_threadsafe(self._broker.shutdown(), self._loop).result(15)
            self._broker = None

    def stop(self) -> None:
        self.shutdown_broker()
        self._loop.call_soon_threadsafe(self._loop.stop)
        self._thread.join(5)


def wait_for(cond, timeout: float = 10.0, interval: float = 0.05):
    end = time.time() + timeout
    while time.time() < end:
        v = cond()
        if v:
            return v
        time.sleep(interval)
    return cond()