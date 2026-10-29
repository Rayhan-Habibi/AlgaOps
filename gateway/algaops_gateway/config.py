"""Gateway configuration (TOML) with secure defaults.

Defaults bind every network service to 127.0.0.1.  Exposing the API on a LAN
interface requires an API token file for state-changing requests unless
allow_insecure_lan is set explicitly.  Secrets (MQTT password, API token) are
read from files referenced by path; they are never stored in this config,
never logged, and never written into evidence bundles.
"""

from __future__ import annotations

import ipaddress
import os
import tomllib
from dataclasses import dataclass, field, fields, replace
from pathlib import Path
from typing import Any

GATEWAY_DIR = Path(__file__).resolve().parents[1]
REPO_ROOT = GATEWAY_DIR.parent
DEFAULT_POLICY = GATEWAY_DIR / "config" / "policy.default.toml"


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class MqttConfig:
    enabled: bool = True
    host: str = "127.0.0.1"
    port: int = 1883
    client_id: str = "algaops-gateway"
    topic_prefix: str = "algaops/v1"
    keepalive_s: int = 30
    username: str | None = None
    password_file: str | None = None
    tls_ca_file: str | None = None
    reconnect_min_s: int = 1
    reconnect_max_s: int = 60


@dataclass(frozen=True)
class StorageConfig:
    db_path: str = "var/algaops.db"
    checkpoint_file: str = "var/checkpoints.jsonl"
    checkpoint_every_events: int = 500
    checkpoint_every_s: int = 300
    export_dir: str = "var/exports"
    retention_days: int = 0     # 0 keeps everything; N > 0: archive, then prune older rows daily


@dataclass(frozen=True)
class ApiConfig:
    enabled: bool = True
    host: str = "127.0.0.1"
    port: int = 8080
    token_file: str | None = None
    allow_insecure_lan: bool = False
    dashboard_dir: str | None = None


@dataclass(frozen=True)
class EngineConfig:
    library_path: str | None = None
    policy_file: str | None = str(DEFAULT_POLICY)
    tick_interval_s: float = 1.0
    warm_start_windows: int = 1200


@dataclass(frozen=True)
class LoggingConfig:
    level: str = "INFO"
    file: str | None = None
    json: bool = False


@dataclass(frozen=True)
class PbrSourceConfig:
    enabled: bool = False
    path: str | None = None
    permission_confirmed: bool = False
    poll_s: float = 10.0


@dataclass(frozen=True)
class DemoConfig:
    enabled: bool = True
    scenarios_dir: str = str(REPO_ROOT / "simulator" / "scenarios")


@dataclass(frozen=True)
class GatewayConfig:
    gateway_id: str = "gw-01"
    site: str = "lab"
    mqtt: MqttConfig = field(default_factory=MqttConfig)
    storage: StorageConfig = field(default_factory=StorageConfig)
    api: ApiConfig = field(default_factory=ApiConfig)
    engine: EngineConfig = field(default_factory=EngineConfig)
    logging: LoggingConfig = field(default_factory=LoggingConfig)
    pbr: PbrSourceConfig = field(default_factory=PbrSourceConfig)
    demo: DemoConfig = field(default_factory=DemoConfig)
    base_dir: str = "."

    def resolve(self, p: str | None) -> Path | None:
        if p is None:
            return None
        path = Path(p)
        return path if path.is_absolute() else (Path(self.base_dir) / path).resolve()

    def api_token(self) -> str | None:
        p = self.resolve(self.api.token_file)
        if p is None:
            return None
        token = p.read_text(encoding="utf-8").strip()
        if len(token) < 16:
            raise ConfigError("API token must be at least 16 characters")
        return token


SECTIONS = {"mqtt": MqttConfig, "storage": StorageConfig, "api": ApiConfig, "engine": EngineConfig,
            "logging": LoggingConfig, "pbr": PbrSourceConfig, "demo": DemoConfig}


def _build(cls, data: dict[str, Any], section: str):
    names = {f.name for f in fields(cls)}
    unknown = set(data) - names
    if unknown:
        raise ConfigError(f"[{section}] unknown key(s): {', '.join(sorted(unknown))}")
    return cls(**data)


def is_loopback(host: str) -> bool:
    if host in ("localhost",):
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def validate(cfg: GatewayConfig) -> GatewayConfig:
    for name, port in (("mqtt.port", cfg.mqtt.port), ("api.port", cfg.api.port)):
        if not 0 < port < 65536:
            raise ConfigError(f"{name} must be in 1..65535")
    if cfg.engine.tick_interval_s <= 0 or cfg.engine.tick_interval_s > 60:
        raise ConfigError("engine.tick_interval_s must be in (0, 60]")
    days = cfg.storage.retention_days
    if isinstance(days, bool) or not isinstance(days, int) or days < 0:
        raise ConfigError("storage.retention_days must be a whole number of days >= 0 "
                          "(0 keeps everything)")
    if cfg.api.enabled and not is_loopback(cfg.api.host):
        if not cfg.api.token_file and not cfg.api.allow_insecure_lan:
            raise ConfigError(
                f"api.host={cfg.api.host} is not a loopback address: set api.token_file "
                "(recommended) or api.allow_insecure_lan = true explicitly")
    if cfg.pbr.enabled and not cfg.pbr.permission_confirmed:
        raise ConfigError("pbr.enabled requires pbr.permission_confirmed = true (read-only access "
                          "to PBR data must be approved by the PBR owner)")
    if cfg.logging.level.upper() not in ("DEBUG", "INFO", "WARNING", "ERROR"):
        raise ConfigError("logging.level must be DEBUG, INFO, WARNING or ERROR")
    return cfg


ENV_OVERRIDES = {
    "ALGAOPS_DB": ("storage", "db_path", str),
    "ALGAOPS_CHECKPOINTS": ("storage", "checkpoint_file", str),
    "ALGAOPS_MQTT_HOST": ("mqtt", "host", str),
    "ALGAOPS_MQTT_PORT": ("mqtt", "port", int),
    "ALGAOPS_API_HOST": ("api", "host", str),
    "ALGAOPS_API_PORT": ("api", "port", int),
    "ALGAOPS_POLICY": ("engine", "policy_file", str),
    "ALGAOPS_LIB": ("engine", "library_path", str),
}


def load_config(path: str | Path | None = None, env: dict[str, str] | None = None
                ) -> GatewayConfig:
    env = os.environ if env is None else env
    data: dict[str, Any] = {}
    base_dir = Path.cwd()
    if path is not None:
        p = Path(path)
        # utf-8-sig: Windows editors (and PowerShell 5.1) prepend a BOM that TOML rejects
        data = tomllib.loads(p.read_text(encoding="utf-8-sig"))
        base_dir = p.resolve().parent
    top = {k: v for k, v in data.items() if not isinstance(v, dict)}
    unknown_top = set(top) - {"gateway_id", "site"}
    if unknown_top:
        raise ConfigError(f"unknown top-level key(s): {', '.join(sorted(unknown_top))}")
    unknown_sections = {k for k, v in data.items() if isinstance(v, dict)} - set(SECTIONS)
    if unknown_sections:
        raise ConfigError(f"unknown section(s): {', '.join(sorted(unknown_sections))}")
    kwargs: dict[str, Any] = dict(top)
    for name, cls in SECTIONS.items():
        kwargs[name] = _build(cls, dict(data.get(name, {})), name)
    cfg = GatewayConfig(base_dir=str(base_dir), **kwargs)
    for var, (section, key, typ) in ENV_OVERRIDES.items():
        if var in env and env[var] != "":
            sec = getattr(cfg, section)
            try:
                value = typ(env[var])
            except ValueError:
                raise ConfigError(f"environment variable {var} has an invalid value") from None
            cfg = replace(cfg, **{section: replace(sec, **{key: value})})
    return validate(cfg)
