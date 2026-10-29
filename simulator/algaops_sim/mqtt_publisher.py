"""Real-time MQTT publishing of a scenario or fixture (paho-mqtt 2.x).

Timestamps are re-based to the current clock and messages are sent at their
scheduled times (1x speed), because faster-than-real-time replay would make
every window look stale or future-dated to the gateway's freshness checks.
Offline evaluation (gateway `evaluate`) is instant instead.
"""

from __future__ import annotations

import json
import logging
import threading
import time
from collections.abc import Callable, Iterable

import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

log = logging.getLogger(__name__)


class Publisher:
    def __init__(self, host: str = "127.0.0.1", port: int = 1883, client_id: str = "algaops-sim",
                 username: str | None = None, password: str | None = None,
                 lwt_topic: str | None = None, lwt_payload: bytes | None = None) -> None:
        self.client = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2,
                                  client_id=client_id, protocol=mqtt.MQTTv311)
        if username:
            self.client.username_pw_set(username, password)
        if lwt_topic and lwt_payload:
            self.client.will_set(lwt_topic, lwt_payload, qos=1, retain=True)
        self._connected = threading.Event()
        self.client.on_connect = lambda c, u, f, rc, p: self._connected.set() \
            if not rc.is_failure else None
        self.host, self.port = host, port

    def __enter__(self) -> Publisher:
        self.client.connect(self.host, self.port, keepalive=30)
        self.client.loop_start()
        if not self._connected.wait(10):
            raise ConnectionError(f"MQTT broker {self.host}:{self.port} not reachable")
        return self

    def __exit__(self, *exc) -> None:
        self.client.disconnect()
        self.client.loop_stop()

    def publish(self, topic: str, payload: bytes, retain: bool = False, qos: int = 1) -> None:
        info = self.client.publish(topic, payload, qos=qos, retain=retain)
        info.wait_for_publish(10)


def publish_schedule(items: Iterable[tuple[int, str, bytes, bool]], pub: Publisher,
                     stop: threading.Event | None = None,
                     progress: Callable[[int, int], None] | None = None) -> int:
    """Publish (t_abs_ms, topic, payload, retain) items at their wall-clock times."""
    items = sorted(items, key=lambda x: x[0])
    sent = 0
    for t_abs, topic, payload, retain in items:
        while True:
            if stop is not None and stop.is_set():
                return sent
            delay = t_abs / 1000.0 - time.time()
            if delay <= 0:
                break
            time.sleep(min(delay, 0.25))
        pub.publish(topic, payload, retain=retain)
        sent += 1
        if progress:
            progress(sent, len(items))
    return sent


def offline_status(node_id: str, prefix: str = "algaops/v1") -> tuple[str, bytes]:
    return (f"{prefix}/nodes/{node_id}/status",
            json.dumps({"schema": "algaops.node_status.v1", "node_id": node_id,
                        "state": "offline", "source": "simulator"},
                       separators=(",", ":")).encode())
