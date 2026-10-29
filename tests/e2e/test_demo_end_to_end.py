"""Vertical slice on live synthetic telemetry: simulator -> MQTT -> gateway ->
C core -> SQLite -> audit chain -> HTTP API.  Outcomes are checked per
scenario against what the injected fault should produce."""

from collections import Counter

from e2e_support import SCENARIOS, TOKEN, WINDOWS

AUTH = {"Authorization": f"Bearer {TOKEN}"}


def windows_of(system, node):
    return [d for d in system.decisions(node) if d["kind"] == "window"]


def test_every_published_window_is_stored_decided_and_audited(system):
    with system.db() as c:
        for node in SCENARIOS:
            n = c.execute("SELECT COUNT(*) FROM windows WHERE node_id = ?", (node,)).fetchone()[0]
            assert n == system.expected_windows(node), node
        undecided = c.execute("SELECT COUNT(*) FROM windows w LEFT JOIN diagnostics d "
                              "ON d.window_id = w.id WHERE d.id IS NULL").fetchone()[0]
        unaudited = c.execute("SELECT COUNT(*) FROM diagnostics WHERE audit_seq IS NULL"
                              ).fetchone()[0]
    assert undecided == 0 and unaudited == 0
    rep = system.http.post("/api/audit/verify", headers=AUTH).json()
    assert rep["ok"], rep["issues"][:5]
    assert rep["windows_checked"] >= sum(system.expected_windows(n) for n in SCENARIOS)


def test_status_api_reports_the_live_system(system):
    st = system.http.get("/api/status").json()
    assert st["mqtt"]["state"] == "connected"
    assert st["data"]["label"] == "DEMO / SYNTHETIC"
    lat = st["pipeline"]["latency_ms"]
    assert lat["samples"] > 0 and lat["p95"] is not None and lat["p95"] < 1000
    nodes = {n["node_id"]: n for n in system.http.get("/api/nodes").json()["nodes"]}
    assert set(SCENARIOS) <= set(nodes)
    assert {nodes[n]["data_source"] for n in SCENARIOS} == {"simulator"}


def test_healthy_node_raises_no_alert(system):
    assert system.alerts("e2e-normal", system.alerts_before_timeout) == []
    out = Counter(d["outcome"] for d in windows_of(system, "e2e-normal"))
    assert out["CHECK_SENSOR"] == 0
    assert out["NORMAL"] + out["NOT_EVALUATED"] >= 0.95 * WINDOWS, out


def test_equipment_fault_on_trusted_data_is_check_equipment(system):
    ds = windows_of(system, "e2e-equipment")
    flagged = [d for d in ds if d["outcome"] == "CHECK_EQUIPMENT"]
    assert flagged and all(d["trust"]["trusted"] for d in flagged)
    assert min(d["seq"] for d in flagged) in range(25, 32), "fault starts at window 25"
    assert all(d["outcome"] == "CHECK_EQUIPMENT" for d in ds if d["seq"] >= 32)
    alerts = system.alerts("e2e-equipment", system.alerts_before_timeout)
    assert [(a["kind"], a["status"]) for a in alerts] == [("CHECK_EQUIPMENT", "open")]


def test_frozen_accelerometer_is_check_sensor_never_check_equipment(system):
    ds = windows_of(system, "e2e-stuck")
    stuck = [d for d in ds if d["seq"] >= 26]
    assert stuck and all(d["outcome"] == "CHECK_SENSOR" for d in stuck)
    assert all("VIB_STUCK" in d["trust"]["flags"] for d in stuck)
    assert not any(d["outcome"] == "CHECK_EQUIPMENT" for d in ds)
    alerts = system.alerts("e2e-stuck", system.alerts_before_timeout)
    assert [(a["kind"], a["status"]) for a in alerts] == [("CHECK_SENSOR", "open")]


def test_transport_faults_are_each_detected(system):
    ds = system.decisions("e2e-transport")
    flags = Counter(f for d in ds for f in d["trust"]["flags"])
    for f in ("DUPLICATE_SEQ", "OUT_OF_ORDER_SEQ", "BAD_TIMESTAMP", "MALFORMED", "MISSING_PACKET"):
        assert flags[f] >= 1, (f, flags)
    for d in ds:
        if set(d["trust"]["flags"]) & {"DUPLICATE_SEQ", "OUT_OF_ORDER_SEQ", "BAD_TIMESTAMP",
                                       "MALFORMED"}:
            assert d["outcome"] == "CHECK_SENSOR", d["reason"]
    with system.db() as c:
        log = Counter(r[0] for r in c.execute(
            "SELECT status FROM ingest_log WHERE node_id = 'e2e-transport'"))
        gaps = c.execute("SELECT SUM(missing) FROM gaps WHERE node_id = 'e2e-transport'"
                         ).fetchone()[0]
    assert log["duplicate_delivery"] >= 1, "exact MQTT redelivery dropped idempotently"
    assert log["rejected"] == 1, "invalid JSON rejected"
    assert gaps >= 2, "dropped windows are recorded as gaps, never as valid data"
    # Isolated transport faults do not confirm an alert (policy: 3 windows).
    assert system.alerts("e2e-transport", system.alerts_before_timeout) == []


def test_dashboard_is_served_by_the_gateway_with_a_strict_csp(system):
    r = system.http.get("/")
    assert r.status_code == 200 and "AlgaOps Assurance" in r.text
    csp = r.headers["content-security-policy"]
    assert "default-src 'self'" in csp and "frame-ancestors 'none'" in csp
    assert r.headers["x-content-type-options"] == "nosniff"
