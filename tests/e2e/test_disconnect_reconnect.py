"""Transport interruptions during the live run:

* e2e-outage loses Wi-Fi for ~25 s, buffers its windows and flushes them on
  reconnect: the gateway raises a stream timeout, marks late windows STALE and
  recovers;
* the gateway itself is restarted 10 s into the run: the persistent MQTT
  session queues the nodes' QoS 1 messages, nothing is lost or duplicated and
  the audit chain continues across the restart.
"""

from e2e_support import TOKEN, WINDOWS

AUTH = {"Authorization": f"Bearer {TOKEN}"}
SEQ_FLAGS = {"MISSING_PACKET", "DUPLICATE_SEQ", "OUT_OF_ORDER_SEQ", "REBOOT", "STALE",
             "LOW_COMPLETENESS", "STREAM_TIMEOUT"}


def test_node_outage_raises_a_stream_timeout_that_clears_after_recovery(system):
    ds = system.decisions("e2e-outage")
    windows = [d for d in ds if d["kind"] == "window"]
    timeouts = [d for d in ds if d["kind"] == "stream_timeout"]
    flushed = [d for d in windows if d["received_ms"] > timeouts[0]["decided_ms"]]
    assert timeouts and flushed, "the timeout fired during the outage, before the flush"
    assert sorted(d["seq"] for d in windows) == list(range(1, WINDOWS + 1)), \
        "every buffered window was delivered"
    stale = [d for d in windows if "STALE" in d["trust"]["flags"]]
    assert stale and all(d["outcome"] == "CHECK_SENSOR" for d in stale)
    assert all(d["outcome"] in ("NORMAL", "NOT_EVALUATED") for d in windows[-5:])
    alerts = system.alerts("e2e-outage", system.alerts_before_timeout)
    assert [(a["kind"], a["status"]) for a in alerts] == [("CHECK_SENSOR", "cleared")]


def test_every_silent_node_gets_a_stream_timeout_alert(system):
    for node in ("e2e-normal", "e2e-transport"):
        alerts = system.alerts(node, system.alerts_after_timeout)
        assert [(a["kind"], a["status"]) for a in alerts] == [("CHECK_SENSOR", "open")], node


def test_gateway_restart_loses_no_message_and_keeps_one_chain(system):
    down, up = system.restart_ms
    assert up > down > system.t0_ms
    with system.db() as c:
        types = [r[0] for r in c.execute("SELECT type FROM audit_events ORDER BY seq")]
        received = [r[0] for r in c.execute(
            "SELECT received_ms FROM windows WHERE node_id = 'e2e-normal' ORDER BY seq")]
    assert types.count("gateway.started") == 2 and types.count("gateway.stopped") >= 1
    assert any(down <= t for t in received) and any(t < down for t in received), \
        "the restart happened in the middle of the stream"
    for node in ("e2e-normal", "e2e-stuck", "e2e-equipment"):
        seqs = [d["seq"] for d in system.decisions(node) if d["kind"] == "window"]
        assert sorted(seqs) == list(range(1, WINDOWS + 1)), node
    for d in system.decisions("e2e-normal"):
        if d["kind"] == "window":
            assert not set(d["trust"]["flags"]) & SEQ_FLAGS, (d["seq"], d["trust"]["flags"])
    rep = system.http.post("/api/audit/verify", headers=AUTH).json()
    assert rep["ok"], rep["issues"][:5]
    assert rep["checkpoints_checked"] >= 2
