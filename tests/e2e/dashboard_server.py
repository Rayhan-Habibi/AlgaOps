"""Gateway API + built dashboard on a scratch database pre-filled with
synthetic scenarios rebased to the current time (DEMO / SYNTHETIC).

Used by the Playwright suite (dashboard/playwright.config.ts) and for a quick
look at the UI without MQTT:

    python tests/e2e/dashboard_server.py --port 8099 [--mqtt-port 1883]

With --mqtt-port the gateway also subscribes to that broker, so live replays
from the dashboard flow through the normal MQTT path.
"""

from __future__ import annotations

import argparse
import dataclasses
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
for p in (ROOT / "gateway", ROOT / "simulator"):
    sys.path.insert(0, str(p))

from algaops_sim.replay import generate_stream  # noqa: E402
from algaops_sim.scenario import load_scenario  # noqa: E402

from algaops_gateway.app import GatewayApp  # noqa: E402
from algaops_gateway.config import (  # noqa: E402
    ApiConfig,
    GatewayConfig,
    MqttConfig,
    StorageConfig,
)

DEMO_NODES = [("normal_steady", "sim-pump-01"), ("stuck_vibration", "sim-pump-02"),
              ("equipment_vibration_anomaly", "sim-pump-03")]
BASELINE_CSV = ROOT / "algaops-assurance" / "data" / "v2" / "baseline_modes.csv"


def build_app(port: int, workdir: Path, mqtt_port: int | None) -> GatewayApp:
    cfg = GatewayConfig(
        gateway_id="demo-gw", site="lab (demo)", base_dir=str(workdir),
        mqtt=MqttConfig(enabled=mqtt_port is not None, port=mqtt_port or 1883,
                        client_id=f"algaops-demo-{port}"),
        storage=StorageConfig(db_path="demo.db", checkpoint_file="checkpoints.jsonl",
                              export_dir="exports"),
        api=ApiConfig(port=port, dashboard_dir=str(ROOT / "dashboard" / "dist")))
    return GatewayApp(cfg)


def populate(app: GatewayApp) -> None:
    """Replay the demo scenarios so that they end just before 'now'.  The
    pipeline clock follows the replayed receive times, so the stored decision
    latencies stay meaningful (they are not real measurements and are cleared)."""
    from algaops_gateway.storage.database import now_ms
    pipe = app.pipeline
    now = now_ms()
    sim_t = {"t": now}
    pipe.clock = lambda: sim_t["t"]
    for name, node in DEMO_NODES:
        pipe.import_baseline_csv(node, str(BASELINE_CSV), activate=True)
        sc = dataclasses.replace(load_scenario(name), node_id=node)
        dur = sc.duration_windows * sc.window_ms
        events, _ = generate_stream(sc, live=True, now_ms=now - dur - 2000)
        for e in events:
            sim_t["t"] = e.t_ms + 2
            pipe.handle_message(e.topic, e.payload, e.t_ms)
    pipe.clock = now_ms
    pipe._maybe_checkpoint(force=True)
    pipe.stats.latencies_ms.clear()
    pipe.stats.last_latency_ms = None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--mqtt-port", type=int)
    ap.add_argument("--workdir")
    args = ap.parse_args()
    workdir = Path(args.workdir) if args.workdir else Path(tempfile.mkdtemp(prefix="algaops_demo_"))
    workdir.mkdir(parents=True, exist_ok=True)
    app = build_app(args.port, workdir, args.mqtt_port)
    app.pipeline.open()
    populate(app)
    app.pipeline.start_worker()
    from algaops_gateway.storage.database import now_ms
    app.ctx.started_ms = now_ms()
    if app.mqtt:
        app.mqtt.start()
    import uvicorn
    print(f"AlgaOps demo gateway on http://127.0.0.1:{args.port}/ (data in {workdir})", flush=True)
    try:
        uvicorn.Server(uvicorn.Config(app.app, host="127.0.0.1", port=args.port, log_config=None,
                                      access_log=False, lifespan="off")).run()
    finally:
        app.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
