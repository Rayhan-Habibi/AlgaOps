"""Shared pieces of the end-to-end suite (see conftest.py): the scenario set,
an in-process gateway with a real HTTP server, and the live System under test.
A separate module so that test files never import conftest directly."""

from __future__ import annotations

import ipaddress
import json
import sqlite3
import sys
import threading
import time
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path

import httpx
from algaops_sim.mqtt_publisher import Publisher, publish_schedule
from algaops_sim.replay import generate_stream
from algaops_sim.scenario import from_dict
from gwtest import DevBroker, free_port, wait_for

ROOT = Path(__file__).resolve().parents[2]
BASELINE_CSV = ROOT / "algaops-assurance" / "data" / "v2" / "baseline_modes.csv"
TOKEN = "e2e-api-token-5f1c0b7e9a2d"
WINDOWS = 50
RESTART_AT_S = 10.0          # gateway stopped ~10 s into the run ...
RESTART_DOWN_S = 3.0         # ... for 3 s: queued QoS 1 messages stay fresh (< 10 s)

SCENARIOS = {
    "e2e-normal": {"seed": 4101},
    "e2e-stuck": {"seed": 4102, "sensor_faults": [
        {"type": "stuck_vibration", "start": 25, "end": WINDOWS, "mode": "frozen"}]},
    "e2e-equipment": {"seed": 4103, "equipment_faults": [
        {"type": "vibration_increase", "start": 24, "end": WINDOWS, "factor": 3.0,
         "ramp_windows": 6}]},
    "e2e-outage": {"seed": 4104, "network": [
        {"type": "disconnect", "start": 18, "end": 37, "buffer_capacity": 300,
         "flush_interval_ms": 20}]},
    "e2e-transport": {"seed": 4105, "network": [
        {"type": "duplicate_exact", "at": 12},
        {"type": "duplicate_conflict", "at": 18},
        {"type": "reorder", "at": 24},
        {"type": "malformed", "at": 30, "kind": "invalid_json"},
        {"type": "bad_timestamp", "start": 36, "end": 37},
        {"type": "drop", "at": 42, "count": 2}]},
}

# ---- every outbound connection of this process is recorded -----------------
_CONNECTS: list[object] = []


def _audit(event: str, args: tuple) -> None:
    if event == "socket.connect":
        _CONNECTS.append(args[1])


sys.addaudithook(_audit)


def non_loopback(addresses: list[object]) -> list[object]:
    bad = []
    for a in addresses:
        host = a[0] if isinstance(a, tuple) else a
        if isinstance(host, bytes):
            host = host.decode(errors="replace")
        if host == "localhost":
            continue
        try:
            if ipaddress.ip_address(str(host).split("%")[0]).is_loopback:
                continue
        except ValueError:
            pass
        bad.append(a)
    return bad


# ---- gateway in this process: pipeline, MQTT ingest and a real HTTP server --
class Gateway:
    def __init__(self, cfg) -> None:
        self.cfg = cfg
        self.app = None
        self.server = None
        self.thread: threading.Thread | None = None

    def start(self, require_mqtt: bool = True) -> None:
        import uvicorn

        from algaops_gateway.app import GatewayApp
        self.app = GatewayApp(self.cfg)
        self.app.start()
        self.server = uvicorn.Server(uvicorn.Config(
            self.app.app, host=self.cfg.api.host, port=self.cfg.api.port, log_config=None,
            access_log=False, lifespan="off"))
        self.thread = threading.Thread(target=self.server.run, name="e2e-api", daemon=True)
        self.thread.start()
        if not wait_for(lambda: self.server.started, 20):
            raise RuntimeError("gateway API did not start")
        if require_mqtt and not wait_for(lambda: self.app.mqtt.state == "connected", 20):
            raise RuntimeError("gateway did not connect to the broker")

    def stop(self) -> None:
        if self.server is not None:
            self.server.should_exit = True
            self.thread.join(15)
            self.server = None
        if self.app is not None:
            self.app.stop()
            self.app = None


@dataclass
class System:
    workdir: Path
    cfg: object
    gateway: Gateway
    http: httpx.Client
    t0_ms: int
    restart_ms: tuple[int, int] = (0, 0)
    events: dict[str, list] = field(default_factory=dict)
    windows: dict[str, list] = field(default_factory=dict)
    alerts_before_timeout: list[dict] = field(default_factory=list)
    alerts_after_timeout: list[dict] = field(default_factory=list)
    connects: list[object] = field(default_factory=list)
    broker: object = None

    def close(self) -> None:
        self.http.close()
        self.gateway.stop()
        if self.broker is not None:
            self.broker.stop()

    @property
    def db_path(self) -> Path:
        return self.cfg.resolve(self.cfg.storage.db_path)

    @property
    def checkpoint_path(self) -> Path:
        return self.cfg.resolve(self.cfg.storage.checkpoint_file)

    @contextmanager
    def db(self) -> Iterator[sqlite3.Connection]:
        c = sqlite3.connect(f"file:{self.db_path.as_posix()}?mode=ro", uri=True, timeout=10)
        c.row_factory = sqlite3.Row
        try:
            yield c
        finally:
            c.close()

    def decisions(self, node: str | None = None) -> list[dict]:
        with self.db() as c:
            q = "SELECT id, window_id, decision_json FROM diagnostics"
            rows = c.execute(q + (" WHERE node_id = ?" if node else "") + " ORDER BY id",
                             (node,) if node else ()).fetchall()
        return [json.loads(r["decision_json"]) | {"id": r["id"], "window_id": r["window_id"]}
                for r in rows]

    def labels(self, node: str) -> dict[tuple[int, int, int], list]:
        """(boot_id, seq, t_end_ms) of every published window -> its ground-truth labels."""
        out: dict[tuple[int, int, int], list] = {}
        for e in self.events[node]:
            if e.kind != "telemetry" or e.label is None:
                continue
            try:
                doc = json.loads(e.payload)
            except ValueError:
                continue
            out.setdefault((doc["boot_id"], doc["seq"], doc["t_end_ms"]), []).append(e.label)
        return out

    def expected_windows(self, node: str) -> int:
        n = 0
        for e in self.events[node]:
            if e.kind != "telemetry" or e.redelivery:
                continue
            try:
                json.loads(e.payload)
            except ValueError:
                continue
            n += 1
        return n

    def alerts(self, node: str, snapshot: list[dict]) -> list[dict]:
        return [a for a in snapshot if a["node_id"] == node]


def _scenario(node: str, spec: dict):
    return from_dict({"schema": "algaops.scenario.v1", "name": node.replace("-", "_"),
                      "description": "end-to-end test stream", "node_id": node,
                      "duration_windows": WINDOWS, **spec})


def _password_file(path: Path) -> None:
    from pwdlib import PasswordHash
    from pwdlib.hashers.argon2 import Argon2Hasher
    h = PasswordHash((Argon2Hasher(),))
    path.write_text(f"gateway:{h.hash('gw-e2e-secret')}\nsim:{h.hash('sim-e2e-secret')}\n",
                    encoding="utf-8")


def start_system(work: Path) -> System:
    """Start broker and gateway, publish every scenario in real time (with one
    gateway restart) and wait for the stream timeouts that follow."""
    from algaops_gateway.config import (
        ApiConfig,
        EngineConfig,
        GatewayConfig,
        MqttConfig,
        StorageConfig,
    )

    _password_file(work / "broker_passwd")
    (work / "gw_password").write_text("gw-e2e-secret", encoding="utf-8")
    (work / "api_token").write_text(TOKEN, encoding="utf-8")
    broker_port = free_port()
    broker = DevBroker(broker_port, password_file=str(work / "broker_passwd"))
    broker.start()
    cfg = GatewayConfig(
        gateway_id="e2e-gw", site="lab (e2e)", base_dir=str(work),
        mqtt=MqttConfig(port=broker_port, client_id="algaops-gateway-e2e", username="gateway",
                        password_file="gw_password", reconnect_min_s=1, reconnect_max_s=2),
        storage=StorageConfig(db_path="e2e.db", checkpoint_file="checkpoints.jsonl",
                              export_dir="exports", checkpoint_every_events=100,
                              checkpoint_every_s=15),
        api=ApiConfig(port=free_port(), token_file="api_token"),
        engine=EngineConfig(tick_interval_s=0.5))
    gw = Gateway(cfg)
    try:
        gw.start()
    except BaseException:
        gw.stop()
        broker.stop()
        raise
    http = httpx.Client(base_url=f"http://127.0.0.1:{cfg.api.port}", timeout=60)

    sysm = System(workdir=work, cfg=cfg, gateway=gw, http=http, t0_ms=0, broker=broker)
    try:
        _run(sysm, broker_port)
    except BaseException:
        sysm.close()
        raise
    return sysm


def _run(sysm: System, broker_port: int) -> None:
    gw, http = sysm.gateway, sysm.http
    auth = {"Authorization": f"Bearer {TOKEN}"}
    csv_text = BASELINE_CSV.read_text(encoding="utf-8")
    for node in SCENARIOS:
        r = http.post("/api/baselines/import", headers=auth,
                      json={"node_id": node, "csv": csv_text, "activate": True})
        r.raise_for_status()
    _CONNECTS.clear()
    t0 = sysm.t0_ms = time.time_ns() // 1_000_000 + 500
    for node, spec in SCENARIOS.items():
        events, windows = generate_stream(_scenario(node, spec), live=True, now_ms=t0)
        sysm.events[node], sysm.windows[node] = events, windows

    errors: list[BaseException] = []

    def run(node: str) -> None:
        try:
            with Publisher("127.0.0.1", broker_port, client_id=f"sim-{node}", username="sim",
                           password="sim-e2e-secret") as pub:
                publish_schedule([(e.t_ms, e.topic, e.payload, e.kind == "status")
                                  for e in sysm.events[node]], pub)
        except BaseException as exc:  # pragma: no cover - reported below
            errors.append(exc)

    def restart() -> None:
        time.sleep(max(0.0, t0 / 1000 + RESTART_AT_S - time.time()))
        down = time.time_ns() // 1_000_000
        gw.stop()
        time.sleep(RESTART_DOWN_S)
        for attempt in range(5):
            try:
                gw.start()
                break
            except (OSError, RuntimeError):     # API port briefly still in use
                gw.stop()
                time.sleep(1 + attempt)
        sysm.restart_ms = (down, time.time_ns() // 1_000_000)

    threads = [threading.Thread(target=run, args=(n,), name=f"pub-{n}") for n in SCENARIOS]
    threads.append(threading.Thread(target=restart, name="restart"))
    for t in threads:
        t.start()
    for t in threads:
        t.join(240)
    if errors:
        raise errors[0]

    def stored(node: str) -> int:
        with sysm.db() as c:
            return c.execute("SELECT COUNT(*) FROM windows WHERE node_id = ?", (node,)
                             ).fetchone()[0]

    assert wait_for(lambda: all(stored(n) >= sysm.expected_windows(n) for n in SCENARIOS), 60), \
        {n: (stored(n), sysm.expected_windows(n)) for n in SCENARIOS}
    sysm.alerts_before_timeout = http.get("/api/alerts", params={"limit": 200}).json()["items"]

    def timed_out(node: str) -> bool:
        with sysm.db() as c:
            last = c.execute("SELECT MAX(received_ms) FROM windows WHERE node_id = ?",
                             (node,)).fetchone()[0]
            return c.execute("SELECT COUNT(*) FROM diagnostics WHERE node_id = ? AND "
                             "kind = 'stream_timeout' AND decided_ms > ?", (node, last)
                             ).fetchone()[0] > 0

    assert wait_for(lambda: all(timed_out(n) for n in SCENARIOS), 60, interval=0.5)
    sysm.alerts_after_timeout = http.get("/api/alerts", params={"limit": 200}).json()["items"]
    sysm.connects = list(_CONNECTS)
