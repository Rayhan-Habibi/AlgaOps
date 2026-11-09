"""Live runtime preflight and failure simulation tests (WP-4A-8)."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

import paho.mqtt.client as mqtt
import pytest
from paho.mqtt.enums import CallbackAPIVersion
from starlette.testclient import TestClient

from algaops_gateway.app import GatewayApp
from algaops_gateway.config import ConfigError, GatewayConfig, load_config, validate
from algaops_gateway.core.libalgaops import find_library

import shutil

def _get_mosquitto_exe() -> Path | None:
    found = shutil.which("mosquitto")
    if found:
        return Path(found)
    win_p = Path("C:/tools/mosquitto/mosquitto.exe")
    return win_p if win_p.is_file() else None

def _get_passwd_exe() -> Path | None:
    found = shutil.which("mosquitto_passwd")
    if found:
        return Path(found)
    win_p = Path("C:/tools/mosquitto/mosquitto_passwd.exe")
    return win_p if win_p.is_file() else None

MOSQUITTO_EXE = _get_mosquitto_exe()
PASSWD_EXE = _get_passwd_exe()


@pytest.mark.skipif(
    not (MOSQUITTO_EXE and PASSWD_EXE and MOSQUITTO_EXE.is_file() and PASSWD_EXE.is_file()),
    reason="Mosquitto broker binary not found"
)
def test_mosquitto_runtime_simulation():
    """Verify live Mosquitto ACLs, password authentication, and topic boundaries."""
    port = 18884
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp = Path(tmp_dir)
        passwd_file = tmp / "passwd"
        acl_file = tmp / "acl.conf"
        conf_file = tmp / "mosquitto.conf"

        subprocess.run([str(PASSWD_EXE), "-c", "-b", str(passwd_file),
                        "algaops-gateway", "gw-token-secret-9999"], check=True)
        subprocess.run([str(PASSWD_EXE), "-b", str(passwd_file),
                        "node-pbr-01", "node01-pass-1111"], check=True)
        subprocess.run([str(PASSWD_EXE), "-b", str(passwd_file),
                        "node-pbr-02", "node02-pass-2222"], check=True)

        acl_file.write_text(
            "user algaops-gateway\n"
            "topic read algaops/v1/#\n"
            "\n"
            "pattern write algaops/v1/nodes/%u/telemetry\n"
            "pattern write algaops/v1/nodes/%u/status\n",
            encoding="utf-8"
        )

        conf_file.write_text(
            f"listener {port} 127.0.0.1\n"
            "allow_anonymous false\n"
            f"password_file {passwd_file.as_posix()}\n"
            f"acl_file {acl_file.as_posix()}\n",
            encoding="utf-8"
        )

        proc = subprocess.Popen([str(MOSQUITTO_EXE), "-c", str(conf_file)],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)
        try:
            # 1. Anonymous access denied
            anon = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="anon", protocol=mqtt.MQTTv311)
            refused = False
            def on_anon(c, u, f, rc, p=None):
                nonlocal refused
                if rc.is_failure: refused = True
            anon.on_connect = on_anon
            try:
                anon.connect("127.0.0.1", port, keepalive=5)
                anon.loop(timeout=0.5)
            except Exception:
                refused = True
            assert refused, "Anonymous connection must be refused"

            # 2. Valid node authentication
            node1 = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="node-pbr-01", protocol=mqtt.MQTTv311)
            node1.username_pw_set("node-pbr-01", "node01-pass-1111")
            connected = False
            def on_n1(c, u, f, rc, p=None):
                nonlocal connected
                if not rc.is_failure: connected = True
            node1.on_connect = on_n1
            node1.connect("127.0.0.1", port, keepalive=10)
            node1.loop_start()
            time.sleep(0.3)
            assert connected, "Node 1 must connect with valid credentials"

            # 3. Gateway subscription
            gw = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="gw", protocol=mqtt.MQTTv311)
            gw.username_pw_set("algaops-gateway", "gw-token-secret-9999")
            msgs = []
            gw.on_message = lambda c, u, m: msgs.append((m.topic, bytes(m.payload)))
            gw.connect("127.0.0.1", port, keepalive=10)
            gw.subscribe("algaops/v1/#", qos=1)
            gw.loop_start()
            time.sleep(0.3)

            # 4. Node 1 publishing to own topic is delivered
            node1.publish("algaops/v1/nodes/node-pbr-01/telemetry", b"telemetry_data", qos=1)
            time.sleep(0.3)
            assert any(t == "algaops/v1/nodes/node-pbr-01/telemetry" for t, _ in msgs)

            # 5. Node 1 publishing to Node 2 topic is dropped by ACL
            msgs.clear()
            node1.publish("algaops/v1/nodes/node-pbr-02/telemetry", b"unauthorized", qos=1)
            time.sleep(0.3)
            assert not any(t == "algaops/v1/nodes/node-pbr-02/telemetry" for t, _ in msgs)

            # 6. Gateway publishing is blocked (read-only)
            msgs.clear()
            gw.publish("algaops/v1/nodes/node-pbr-01/commands", b"cmd", qos=1)
            time.sleep(0.3)
            assert len(msgs) == 0

            node1.loop_stop(); node1.disconnect()
            gw.loop_stop(); gw.disconnect()
        finally:
            proc.terminate()
            proc.wait(timeout=5)


def test_gateway_app_lifecycle_and_api(tmp_path):
    """Verify clean GatewayApp startup, SQLite database creation, API routes, and shutdown."""
    db_file = tmp_path / "test_lifecycle.db"
    chk_file = tmp_path / "checkpoints.jsonl"
    exp_dir = tmp_path / "exports"

    cfg = load_config(None, env={
        "ALGAOPS_DB": str(db_file),
        "ALGAOPS_CHECKPOINTS": str(chk_file),
        "ALGAOPS_API_HOST": "127.0.0.1",
        "ALGAOPS_API_PORT": "8089",
    })
    # Disable live MQTT network listener for this in-process API test
    cfg = GatewayConfig(
        gateway_id="gw-test",
        site="test-site",
        storage=cfg.storage,
        api=cfg.api,
        engine=cfg.engine,
        logging=cfg.logging,
        mqtt=cfg.mqtt.__class__(enabled=False)
    )

    app = GatewayApp(cfg)
    app.start()
    try:
        assert db_file.is_file(), "SQLite database must be created"
        client = TestClient(app.app)

        # Health endpoint
        r = client.get("/api/health")
        assert r.status_code == 200
        assert r.json()["status"] == "ok"

        # Ready endpoint
        r = client.get("/api/ready")
        assert r.status_code == 200
        assert r.json()["ready"] is True
        assert r.json()["problems"] == []

        # Nodes endpoint
        r = client.get("/api/nodes")
        assert r.status_code == 200
        assert isinstance(r.json()["nodes"], list)

        # Audit status endpoint
        r = client.get("/api/audit/status")
        assert r.status_code == 200
        assert "head_seq" in r.json()
        assert "hash_algorithm" in r.json()

        # PBR read-only context endpoint
        r = client.get("/api/pbr/context")
        assert r.status_code == 200
        assert r.json()["enabled"] is False
        assert "read only" in r.json()["note"].lower()
    finally:
        app.stop()


def test_failure_broker_offline_graceful():
    """Verify that an unavailable MQTT broker does not crash the gateway at startup."""
    cfg = load_config(None, env={
        "ALGAOPS_MQTT_HOST": "127.0.0.1",
        "ALGAOPS_MQTT_PORT": "19999",  # Port with no listening broker
    })
    app = GatewayApp(cfg)
    app.start()
    try:
        # Gateway starts cleanly even when broker is unreachable
        assert app.mqtt.state in ("connecting", "disconnected")
        client = TestClient(app.app)
        r = client.get("/api/health")
        assert r.status_code == 200
    finally:
        app.stop()


def test_failure_invalid_config_validation():
    """Verify fail-closed configuration validation for insecure LAN and invalid ports."""
    # Non-loopback host without token_file or allow_insecure_lan
    with pytest.raises(ConfigError, match="not a loopback address"):
        validate(GatewayConfig(api=GatewayConfig().api.__class__(host="192.168.10.1", token_file=None)))

    # Invalid port
    with pytest.raises(ConfigError, match="must be in 1..65535"):
        validate(GatewayConfig(mqtt=GatewayConfig().mqtt.__class__(port=70000)))


def test_failure_c_library_missing():
    """Verify actionable FileNotFoundError when native C library is absent."""
    with pytest.raises(FileNotFoundError, match="libalgaops not found"):
        find_library("nonexistent_path/libalgaops.so")
