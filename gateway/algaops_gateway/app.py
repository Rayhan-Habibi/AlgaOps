"""Gateway application wiring: core library, policy, storage, pipeline, MQTT
ingest, optional read-only PBR context and the local HTTP API."""

from __future__ import annotations

import logging
import threading
from pathlib import Path

from .api.events import EventBus
from .api.server import AppContext, create_app
from .audit.checkpoint import CheckpointWriter
from .config import REPO_ROOT, GatewayConfig
from .core.engine import load_policy
from .core.libalgaops import load_library
from .ingest.mqtt_client import MqttIngest
from .pipeline import Pipeline
from .sources.pbr_readonly_csv import PbrReadonlyCsvSource
from .storage.database import Database, now_ms

log = logging.getLogger(__name__)


class GatewayApp:
    def __init__(self, config: GatewayConfig) -> None:
        self.config = config
        self.lib = load_library(config.resolve(config.engine.library_path))
        self.policy = load_policy(self.lib, config.resolve(config.engine.policy_file))
        log.info("core %s loaded from %s; policy %s (%s)", self.lib.version, self.lib.path,
                 self.policy.meta.get("name"), self.policy.hash[:16])
        self.db = Database(config.resolve(config.storage.db_path))
        self.events = EventBus()
        self.pipeline = Pipeline(
            lib=self.lib, policy=self.policy, db=self.db, gateway_id=config.gateway_id,
            topic_prefix=config.mqtt.topic_prefix, events=self.events,
            checkpoint_writer=CheckpointWriter(config.resolve(config.storage.checkpoint_file)),
            checkpoint_every_events=config.storage.checkpoint_every_events,
            checkpoint_every_s=config.storage.checkpoint_every_s,
            tick_interval_s=config.engine.tick_interval_s,
            warm_start_windows=config.engine.warm_start_windows,
            retention_days=config.storage.retention_days,
            archive_dir=config.resolve(config.storage.export_dir))
        self.mqtt = None
        if config.mqtt.enabled:
            m = config.mqtt
            self.mqtt = MqttIngest(
                m.host, m.port, m.client_id, m.topic_prefix, self._on_mqtt, keepalive=m.keepalive_s,
                username=m.username, password_file=str(config.resolve(m.password_file))
                if m.password_file else None,
                tls_ca_file=str(config.resolve(m.tls_ca_file)) if m.tls_ca_file else None,
                reconnect_min_s=m.reconnect_min_s, reconnect_max_s=m.reconnect_max_s,
                on_state=lambda s: self.events.publish("gateway", {"mqtt": s}))
        self.pbr = None
        if config.pbr.enabled:
            self.pbr = PbrReadonlyCsvSource(config.resolve(config.pbr.path),
                                            config.pbr.permission_confirmed)
        dist = config.resolve(config.api.dashboard_dir) if config.api.dashboard_dir else \
            REPO_ROOT / "dashboard" / "dist"
        self.ctx = AppContext(config=config, lib=self.lib, policy=self.policy, db=self.db,
                              pipeline=self.pipeline, events=self.events, mqtt=self.mqtt,
                              token=config.api_token(), dashboard_dir=Path(dist))
        self.app = create_app(self.ctx)
        self._stop = threading.Event()

    def _on_mqtt(self, topic: str, payload: bytes, received_ms: int, ack) -> None:
        self.pipeline.submit_message(topic, payload, received_ms, ack)

    def _pbr_loop(self) -> None:
        while not self._stop.wait(self.config.pbr.poll_s):
            try:
                self.pbr.poll()
            except Exception:
                log.exception("PBR context poll failed")

    def start(self) -> None:
        self.pipeline.start()
        self.ctx.started_ms = now_ms()
        if self.mqtt:
            self.mqtt.start()
        if self.pbr:
            threading.Thread(target=self._pbr_loop, name="pbr-context", daemon=True).start()

    def stop(self) -> None:
        self._stop.set()
        if self.mqtt:
            self.mqtt.stop()
        self.pipeline.stop()
        self.db.close()

    def serve(self) -> None:
        import uvicorn
        self.start()
        try:
            if self.config.api.enabled:
                server = uvicorn.Server(uvicorn.Config(
                    self.app, host=self.config.api.host, port=self.config.api.port,
                    log_config=None, access_log=False, lifespan="off"))
                server.run()
            else:
                self._stop.wait()
        finally:
            self.stop()
