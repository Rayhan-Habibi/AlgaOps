"""Subscribe-only MQTT client (paho-mqtt 2.x).

- Persistent session (clean_session=False, fixed client id): the broker keeps
  QoS 1 telemetry queued while the gateway is down and redelivers it.
- Manual acknowledgement: a PUBACK is sent only after the pipeline committed
  the message to SQLite, so a crash between receive and commit leads to a
  redelivery (handled idempotently) instead of data loss.
- Automatic reconnect with exponential back-off (reconnect_delay_set).
- The client never publishes: there are no command topics.
"""

from __future__ import annotations

import logging
import ssl
import threading
import time
from collections.abc import Callable
from pathlib import Path

import paho.mqtt.client as mqtt
from paho.mqtt.enums import CallbackAPIVersion

from .topics import subscriptions

log = logging.getLogger(__name__)

AckFn = Callable[[], None]
MessageHandler = Callable[[str, bytes, int, AckFn], None]


class MqttIngest:
    def __init__(self, host: str, port: int, client_id: str, topic_prefix: str,
                 on_message: MessageHandler, *, keepalive: int = 30,
                 username: str | None = None, password_file: str | None = None,
                 tls_ca_file: str | None = None, reconnect_min_s: int = 1,
                 reconnect_max_s: int = 60, on_state: Callable[[str], None] | None = None) -> None:
        self.host, self.port, self.keepalive = host, port, keepalive
        self.topic_prefix = topic_prefix
        self._on_message_cb = on_message
        self._on_state = on_state
        self.state = "disconnected"
        self.connects = 0
        self.disconnects = 0
        self.last_error: str | None = None
        self._lock = threading.Lock()
        c = mqtt.Client(callback_api_version=CallbackAPIVersion.VERSION2, client_id=client_id,
                        clean_session=False, protocol=mqtt.MQTTv311, manual_ack=True)
        if username:
            password = Path(password_file).read_text(encoding="utf-8").strip() if password_file else None
            c.username_pw_set(username, password)
        if tls_ca_file:
            c.tls_set(ca_certs=tls_ca_file, cert_reqs=ssl.CERT_REQUIRED)
        c.reconnect_delay_set(min_delay=reconnect_min_s, max_delay=reconnect_max_s)
        c.max_inflight_messages_set(20)
        c.on_connect = self._on_connect
        c.on_disconnect = self._on_disconnect
        c.on_message = self._on_message
        c.on_connect_fail = self._on_connect_fail
        self._client = c

    def _set_state(self, state: str) -> None:
        with self._lock:
            self.state = state
        if self._on_state:
            try:
                self._on_state(state)
            except Exception:  # pragma: no cover - observer must not break MQTT
                log.exception("state observer failed")

    # paho callbacks (network thread) ----------------------------------------------
    def _on_connect(self, client, userdata, flags, reason_code, properties) -> None:
        if reason_code.is_failure:
            self.last_error = str(reason_code)
            log.warning("MQTT connect refused: %s", reason_code)
            self._set_state("refused")
            return
        self.connects += 1
        client.subscribe([(t, 1) for t in subscriptions(self.topic_prefix)])
        log.info("MQTT connected to %s:%s (session present: %s)", self.host, self.port,
                 getattr(flags, "session_present", None))
        self._set_state("connected")

    def _on_disconnect(self, client, userdata, flags, reason_code, properties) -> None:
        self.disconnects += 1
        if reason_code is not None and getattr(reason_code, "is_failure", False):
            self.last_error = str(reason_code)
        log.warning("MQTT disconnected (%s); reconnecting with back-off", reason_code)
        self._set_state("disconnected")

    def _on_connect_fail(self, client, userdata) -> None:
        self.last_error = "connection failed"
        self._set_state("disconnected")

    def _on_message(self, client, userdata, msg) -> None:
        received_ms = time.time_ns() // 1_000_000
        mid, qos = msg.mid, msg.qos

        def ack() -> None:
            if qos > 0:
                try:
                    client.ack(mid, qos)
                except Exception:  # pragma: no cover
                    log.exception("MQTT ack failed for mid %s", mid)

        try:
            self._on_message_cb(msg.topic, bytes(msg.payload), received_ms, ack)
        except Exception:
            log.exception("message handler failed for topic %s", msg.topic)

    # lifecycle ------------------------------------------------------------------------
    def start(self) -> None:
        self._set_state("connecting")
        self._client.connect_async(self.host, self.port, keepalive=self.keepalive)
        self._client.loop_start()

    def stop(self) -> None:
        try:
            self._client.disconnect()
        finally:
            self._client.loop_stop()
            self._set_state("stopped")
