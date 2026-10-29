"""Network exposure defaults and read-only guarantees."""

import ast
from pathlib import Path

import pytest

from algaops_gateway.config import ConfigError, load_config

PKG = Path(__file__).resolve().parents[2] / "algaops_gateway"


def test_default_binding_is_loopback():
    cfg = load_config(None, env={})
    assert cfg.api.host == "127.0.0.1" and not cfg.api.allow_insecure_lan


def test_lan_exposure_needs_explicit_decision(tmp_path):
    p = tmp_path / "g.toml"
    p.write_text('[api]\nhost = "0.0.0.0"\n', encoding="utf-8")
    with pytest.raises(ConfigError):
        load_config(p, env={})


def test_example_config_has_no_secrets():
    text = (PKG.parent / "config" / "gateway.example.toml").read_text(encoding="utf-8")
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("#") or "=" not in s:
            continue
        key = s.split("=", 1)[0].strip().lower()
        assert key not in ("password", "token", "secret", "api_key"), line


def test_gateway_code_never_publishes_mqtt():
    """The gateway is read-only towards the plant: no publish() call in its code."""
    offenders = []
    for py in PKG.rglob("*.py"):
        tree = ast.parse(py.read_text(encoding="utf-8"))
        for node in ast.walk(tree):
            if isinstance(node, ast.Attribute) and node.attr in ("publish", "will_set"):
                # EventBus.publish (SSE to the dashboard) and the demo replay publisher
                # (simulated node, routes_replay) are not plant-facing
                if py.name in ("routes_replay.py", "pipeline.py", "app.py", "events.py"):
                    continue
                offenders.append(f"{py.name}:{node.lineno}")
    assert not offenders, offenders


def test_no_actuation_vocabulary_in_policy_keys(lib):
    for key in lib.config_keys:
        for word in ("relay", "actuat", "shutdown", "setpoint", "pump_on", "pump_off"):
            assert word not in key
