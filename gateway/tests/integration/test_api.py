"""HTTP API against a real pipeline and database (no MQTT)."""

import json
from dataclasses import replace

import pytest
from fastapi.testclient import TestClient
from gwtest import feed

from algaops_gateway.api.events import EventBus
from algaops_gateway.api.server import AppContext, create_app
from algaops_gateway.config import GatewayConfig, StorageConfig


@pytest.fixture
def api(lib, policy, pipeline, tmp_path, clock):
    cfg = GatewayConfig(base_dir=str(tmp_path),
                        storage=replace(StorageConfig(), checkpoint_file="checkpoints.jsonl",
                                        export_dir="exports"))
    ctx = AppContext(config=cfg, lib=lib, policy=policy, db=pipeline.db, pipeline=pipeline,
                     events=pipeline.events, started_ms=clock.t, dashboard_dir=tmp_path / "none")
    feed(pipeline, clock, 30)
    feed(pipeline, clock, 4, start_seq=31, samples_valid=10)     # untrusted -> alert
    return TestClient(create_app(ctx)), ctx


def test_health_status_nodes(api):
    c, _ = api
    assert c.get("/api/health").json() == {"status": "ok"}
    st = c.get("/api/status").json()
    assert st["read_only"] is True and st["data"]["label"] == "DEMO / SYNTHETIC"
    assert st["audit"]["head_seq"] > 30 and st["policy"]["hash"]
    nodes = c.get("/api/nodes").json()["nodes"]
    assert nodes[0]["node_id"] == "node-a" and nodes[0]["latest"]["outcome"] == "CHECK_SENSOR"
    assert nodes[0]["open_alert"]["kind"] == "CHECK_SENSOR"
    assert c.get("/api/nodes/none").status_code == 404


def test_ready_reports_problems_without_worker(api):
    c, _ = api
    r = c.get("/api/ready")
    assert r.status_code == 503 and "pipeline worker not running" in r.json()["problems"]


def test_diagnostics_detail_traceability(api):
    c, _ = api
    items = c.get("/api/diagnostics", params={"limit": 5}).json()["items"]
    assert len(items) == 5 and items[0]["outcome"] == "CHECK_SENSOR"
    detail = c.get(f"/api/diagnostics/{items[0]['id']}").json()
    assert detail["window"]["record"]["seq"] == items[0]["seq"]
    assert detail["audit_event"]["type"] == "window.diagnosed"
    assert "INCOMPLETE_WINDOW" in detail["decision"]["trust"]["flags"]
    only = c.get("/api/diagnostics", params={"outcome": "CHECK_SENSOR"}).json()["items"]
    assert all(i["outcome"] == "CHECK_SENSOR" for i in only)
    assert c.get("/api/diagnostics", params={"outcome": "BOGUS"}).status_code == 400


def test_history_trend_packets(api):
    c, _ = api
    tr = c.get("/api/history/trend", params={"node_id": "node-a", "metric": "vib_rms_g",
                                             "since_ms": 0, "until_ms": 2**62}).json()
    assert tr["points"] and tr["markers"][0]["outcome"] == "CHECK_SENSOR"
    assert {m["mode"] for m in tr["modes"]} <= {"OFF", "STARTUP", "STEADY"}
    pk = c.get("/api/history/packets", params={"node_id": "node-a", "since_ms": 0}).json()
    assert pk["counts"]["valid"] == 30 and pk["counts"]["invalid"] == 4
    w = c.get("/api/history/windows", params={"node_id": "node-a", "limit": 3}).json()["items"]
    assert len(w) == 3 and w[-1]["seq"] == 34


def test_alert_feedback_audit_export(api):
    c, ctx = api
    alerts = c.get("/api/alerts").json()["items"]
    aid = alerts[0]["id"]
    r = c.post(f"/api/alerts/{aid}/feedback", json={"verdict": "true_alarm",
                                                    "technician": "JA", "note": "cable"})
    assert r.status_code == 200
    assert c.post(f"/api/alerts/{aid}/feedback", json={"verdict": "x", "technician": "JA"}
                  ).status_code == 422
    detail = c.get(f"/api/alerts/{aid}").json()
    assert detail["feedback"][0]["verdict"] == "true_alarm" and detail["decisions"]
    ver = c.post("/api/audit/verify").json()
    assert ver["ok"] is True and ver["recorded_as_seq"] > 0
    ex = c.post("/api/export/evidence", json={}).json()
    assert ex["manifest"]["verification"]["ok"] and ex["file"].startswith("algaops-evidence-")
    last_cp = ctx.db.writer.execute("SELECT MAX(seq) FROM checkpoints").fetchone()[0]
    assert last_cp == ex["manifest"]["chain"]["head_seq"], "exported head is checkpointed"
    dl = c.get(ex["url"])
    assert dl.status_code == 200 and dl.headers["content-type"] == "application/zip"
    assert c.get("/api/export/files/../../etc/passwd").status_code in (400, 404)
    csvr = c.get("/api/export/csv", params={"table": "diagnostics"})
    assert csvr.status_code == 200 and "flag_names" in csvr.text.splitlines()[0]
    st = c.get("/api/audit/status").json()
    assert st["last_verification"]["ok"] is True and st["event_counts"]["export.created"] == 1


def test_baseline_endpoints(api):
    c, ctx = api
    r = c.post("/api/baselines/capture", json={"node_id": "node-a", "mode": "STEADY",
                                               "windows": 5})
    assert r.status_code == 200
    assert c.get("/api/baselines/capture").json()["sessions"][0]["target"] == 5
    assert c.post("/api/baselines/capture", json={"node_id": "node-a", "mode": "RUNNING",
                                                  "windows": 5}).status_code == 422
    assert c.delete("/api/baselines/capture/node-a").json() == {"cancelled": "node-a"}
    csv_text = (ctx.lib.path.parents[1] / "data" / "baseline_normal.csv").read_text()
    r = c.post("/api/baselines/import", json={"node_id": "node-a", "csv": csv_text})
    assert r.status_code == 400   # 30 v1 rows are not enough under the default policy


def test_security_headers_and_no_control_routes(api):
    c, _ = api
    r = c.get("/api/status")
    assert "default-src 'self'" in r.headers["content-security-policy"]
    assert r.headers["x-frame-options"] == "DENY" and r.headers["cache-control"] == "no-store"
    paths = list(c.app.openapi()["paths"]) + [getattr(r, "path", "") for r in c.app.routes]
    for word in ("pump", "relay", "actuat", "shutdown", "setpoint", "command", "control"):
        assert not any(word in p.lower() for p in paths), word


def test_token_required_when_configured(api):
    c, ctx = api
    ctx.token = "0123456789abcdef-token"
    assert c.post("/api/audit/verify").status_code == 401
    assert c.post("/api/audit/verify", headers={"Authorization": "Bearer wrong"}).status_code == 401
    ok = c.post("/api/audit/verify", headers={"Authorization": "Bearer 0123456789abcdef-token"})
    assert ok.status_code == 200
    assert c.get("/api/status").status_code == 200   # reads stay open on the bound interface


def test_dashboard_fallback_page(api):
    c, _ = api
    r = c.get("/")
    assert r.status_code == 200 and "dashboard has not been built" in r.text
