"""Offline operation: the whole run stayed on this machine, the services listen
on loopback only, the dashboard needs no internet, and the gateway keeps
serving its history while the broker is unreachable."""

import ipaddress
import re
import shutil
import sqlite3

from e2e_support import ROOT, Gateway, non_loopback
from gwtest import free_port, wait_for

from algaops_gateway.config import ApiConfig, GatewayConfig, MqttConfig, StorageConfig


def test_the_run_made_no_connection_outside_this_machine(system):
    assert system.connects, "the MQTT and HTTP connections were observed"
    assert non_loopback(system.connects) == []


def test_api_and_mqtt_endpoints_are_loopback(system):
    sockets = [s for srv in system.gateway.server.servers for s in srv.sockets]
    assert sockets and all(ipaddress.ip_address(s.getsockname()[0]).is_loopback
                           for s in sockets)
    assert ipaddress.ip_address(system.cfg.mqtt.host).is_loopback


def test_dashboard_bundle_references_nothing_outside_the_gateway():
    dist = ROOT / "dashboard" / "dist"
    files = [f for f in dist.rglob("*") if f.suffix in (".html", ".js", ".css", ".webmanifest",
                                                        ".svg")]
    assert files, "dashboard/dist is built"
    for f in files:
        urls = re.findall(r"(?:https?:)?//[a-z0-9.-]+\.[a-z]{2,}[^\s\"'`)]*",
                          f.read_text(encoding="utf-8"))
        assert [u for u in urls if "www.w3.org/2000/svg" not in u] == [], f


def test_gateway_serves_history_while_the_broker_is_unreachable(system, tmp_path):
    shutil.copyfile(system.checkpoint_path, tmp_path / "checkpoints.jsonl")
    src = sqlite3.connect(f"file:{system.db_path.as_posix()}?mode=ro", uri=True)
    dst = sqlite3.connect(tmp_path / "offline.db")
    src.backup(dst)
    src.close()
    dst.close()
    cfg = GatewayConfig(
        gateway_id="e2e-gw", site="lab (offline)", base_dir=str(tmp_path),
        mqtt=MqttConfig(port=free_port(), client_id="algaops-gateway-offline",
                        reconnect_min_s=1, reconnect_max_s=2),
        storage=StorageConfig(db_path="offline.db", checkpoint_file="checkpoints.jsonl"),
        api=ApiConfig(port=free_port()))
    gw = Gateway(cfg)
    gw.start(require_mqtt=False)
    try:
        import httpx
        with httpx.Client(base_url=f"http://127.0.0.1:{cfg.api.port}", timeout=30) as http:
            assert http.get("/api/health").json()["status"] == "ok"
            assert wait_for(lambda: gw.app.mqtt.disconnects + gw.app.mqtt.connects == 0
                            and gw.app.mqtt.state != "connected", 5)
            st = http.get("/api/status").json()
            assert st["mqtt"]["state"] != "connected"
            nodes = {n["node_id"] for n in http.get("/api/nodes").json()["nodes"]}
            assert {"e2e-normal", "e2e-equipment"} <= nodes
            items = http.get("/api/diagnostics", params={"node_id": "e2e-equipment",
                                                         "outcome": "CHECK_EQUIPMENT"}).json()
            assert items["items"], "stored decisions stay available"
            rep = http.post("/api/audit/verify").json()
            assert rep["ok"], rep["issues"][:3]
    finally:
        gw.stop()
