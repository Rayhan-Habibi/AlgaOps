"""Transport failures: broker outage and gateway downtime.

1. Broker restart: the ingest client reconnects with back-off.
2. Gateway down while nodes publish: the persistent session (clean_session=False,
   fixed client id) lets the broker queue QoS 1 messages, delivered on reconnect.
"""

import json
import time

import paho.mqtt.client as mqtt
from gwtest import DevBroker, free_port, make_doc, wait_for
from paho.mqtt.enums import CallbackAPIVersion

from algaops_gateway.audit.checkpoint import CheckpointWriter
from algaops_gateway.audit.verify import verify_database
from algaops_gateway.ingest.mqtt_client import MqttIngest
from algaops_gateway.pipeline import Pipeline


def doc(seq, t0):
    d = make_doc(seq)
    d["t_end_ms"] = t0 + seq * 1280
    d["t_start_ms"] = d["t_end_ms"] - 1280
    return json.dumps(d)


def publisher(port, user=None, password=None):
    p = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id="node-pub")
    if user:
        p.username_pw_set(user, password)
    p.connect("127.0.0.1", port)
    p.loop_start()
    return p


def test_reconnect_after_broker_restart(lib, policy, db, tmp_path):
    port = free_port()
    b = DevBroker(port)
    b.start()
    pipe = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw",
                    checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"))
    pipe.start()
    ing = MqttIngest("127.0.0.1", port, "gw-reconnect", "algaops/v1", pipe.submit_message,
                     reconnect_min_s=1, reconnect_max_s=2)
    ing.start()
    try:
        assert wait_for(lambda: ing.state == "connected", 10)
        b.shutdown_broker()
        assert wait_for(lambda: ing.state != "connected", 10)
        b.start()
        assert wait_for(lambda: ing.state == "connected", 20), "client must reconnect"
        assert ing.connects >= 2 and ing.disconnects >= 1
        t0 = time.time_ns() // 1_000_000 - 10 * 1280
        p = publisher(port)
        for s in range(1, 4):
            p.publish("algaops/v1/nodes/node-a/telemetry", doc(s, t0), qos=1).wait_for_publish(5)
        assert wait_for(lambda: db.writer.execute("SELECT COUNT(*) FROM windows").fetchone()[0]
                        == 3, 15)
        p.loop_stop()
        p.disconnect()
    finally:
        ing.stop()
        pipe.stop()
        b.stop()


def test_messages_queued_while_gateway_down(lib, policy, db, tmp_path):
    # Persistent sessions need authenticated clients on the dev broker (amqtt does not
    # queue for anonymous sessions; Mosquitto in deploy/ uses password auth as well).
    from pwdlib import PasswordHash
    from pwdlib.hashers.argon2 import Argon2Hasher
    h = PasswordHash((Argon2Hasher(),))
    pw = tmp_path / "passwd"
    pw.write_text(f"gateway:{h.hash('gw-secret')}\nnode:{h.hash('node-secret')}\n",
                  encoding="utf-8")
    secret = tmp_path / "gw_password"
    secret.write_text("gw-secret", encoding="utf-8")
    port = free_port()
    b = DevBroker(port, password_file=str(pw))
    b.start()
    pipe = Pipeline(lib=lib, policy=policy, db=db, gateway_id="gw",
                    checkpoint_writer=CheckpointWriter(tmp_path / "cp.jsonl"))
    pipe.start()
    try:
        auth = dict(username="gateway", password_file=str(secret))
        ing = MqttIngest("127.0.0.1", port, "gw-persistent", "algaops/v1", pipe.submit_message,
                         **auth)
        ing.start()
        assert wait_for(lambda: ing.state == "connected", 10)
        ing.stop()                                      # gateway goes down (session kept)
        t0 = time.time_ns() // 1_000_000 - 10 * 1280
        p = publisher(port, "node", "node-secret")
        for s in range(1, 6):
            p.publish("algaops/v1/nodes/node-a/telemetry", doc(s, t0), qos=1).wait_for_publish(5)
        p.loop_stop()
        p.disconnect()
        ing2 = MqttIngest("127.0.0.1", port, "gw-persistent", "algaops/v1", pipe.submit_message,
                          **auth)
        ing2.start()
        got = wait_for(lambda: db.writer.execute("SELECT COUNT(*) FROM windows").fetchone()[0]
                       == 5, 20)
        ing2.stop()
        assert got, "queued QoS 1 messages must be delivered after the gateway reconnects"
        assert [r[0] for r in db.writer.execute("SELECT seq FROM windows ORDER BY id")] == \
            [1, 2, 3, 4, 5]
        assert verify_database(db.writer, pipe.hasher).ok
    finally:
        pipe.stop()
        b.stop()
