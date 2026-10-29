"""MQTT ingest over a real (development) broker: QoS 1, manual ack after commit,
idempotent redelivery, malformed payloads, status topic."""

import json
import time

import paho.mqtt.client as mqtt
import pytest
from gwtest import make_doc, wait_for
from paho.mqtt.enums import CallbackAPIVersion

from algaops_gateway.audit.checkpoint import CheckpointWriter
from algaops_gateway.ingest.mqtt_client import MqttIngest
from algaops_gateway.pipeline import Pipeline


def now_doc(seq, t0):
    d = make_doc(seq)
    d["t_end_ms"] = t0 + seq * 1280
    d["t_start_ms"] = d["t_end_ms"] - 1280
    return d


@pytest.fixture
def live(lib, policy, db, tmp_path, broker):
    host, port, _ = broker
    pipe = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw",
                    checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"),
                    tick_interval_s=0.2)
    pipe.start()
    ing = MqttIngest(host, port, "algaops-gateway-test", "algaops/v1", pipe.submit_message)
    ing.start()
    assert wait_for(lambda: ing.state == "connected", 10)
    pub = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="node-sim")
    pub.connect(host, port)
    pub.loop_start()
    yield pipe, ing, pub
    pub.loop_stop()
    pub.disconnect()
    ing.stop()
    pipe.stop()


def count(pipe, table):
    return pipe.db.writer.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]


def test_telemetry_flows_to_storage(live):
    pipe, ing, pub = live
    t0 = time.time_ns() // 1_000_000 - 20 * 1280
    for s in range(1, 11):
        pub.publish("algaops/v1/nodes/node-a/telemetry", json.dumps(now_doc(s, t0)), qos=1
                    ).wait_for_publish(5)
    assert wait_for(lambda: count(pipe, "windows") == 10, 15)
    rows = pipe.db.writer.execute("SELECT seq FROM windows ORDER BY id").fetchall()
    assert [r[0] for r in rows] == list(range(1, 11)), "order preserved"
    assert pipe.stats.latencies_ms and max(pipe.stats.latencies_ms) < 2000


def test_duplicate_and_malformed_over_mqtt(live):
    pipe, ing, pub = live
    t0 = time.time_ns() // 1_000_000 - 20 * 1280
    raw = json.dumps(now_doc(1, t0))
    for _ in range(3):                                   # retries of the same message
        pub.publish("algaops/v1/nodes/node-a/telemetry", raw, qos=1).wait_for_publish(5)
    pub.publish("algaops/v1/nodes/node-a/telemetry", "not json", qos=1).wait_for_publish(5)
    pub.publish("algaops/v1/nodes/node-a/status", json.dumps(
        {"schema": "algaops.node_status.v1", "node_id": "node-a", "state": "online"}),
        qos=1, retain=True).wait_for_publish(5)
    assert wait_for(lambda: pipe.stats.rejected == 1 and pipe.stats.duplicate_deliveries == 2, 15)
    assert count(pipe, "windows") == 1
    kinds = [r[0] for r in pipe.db.writer.execute("SELECT kind FROM diagnostics ORDER BY id")]
    assert kinds == ["window", "malformed"]
    assert wait_for(lambda: pipe.db.writer.execute(
        "SELECT status_json FROM nodes WHERE node_id='node-a'").fetchone()[0] is not None, 10)


def test_gateway_never_publishes(live):
    """Read-only: the ingest client has no publish path and subscribes only."""
    pipe, ing, pub = live
    assert not hasattr(ing, "publish")
    received = []
    spy = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="spy")
    spy.on_message = lambda c, u, m: received.append(m.topic)
    spy.connect(ing.host, ing.port)
    spy.subscribe("#", qos=1)
    spy.loop_start()
    time.sleep(0.5)
    t0 = time.time_ns() // 1_000_000 - 5 * 1280
    pub.publish("algaops/v1/nodes/node-a/telemetry", json.dumps(now_doc(1, t0)), qos=1
                ).wait_for_publish(5)
    time.sleep(1.0)
    spy.loop_stop()
    spy.disconnect()
    assert received and all(t.startswith("algaops/v1/nodes/node-a/") for t in received)
