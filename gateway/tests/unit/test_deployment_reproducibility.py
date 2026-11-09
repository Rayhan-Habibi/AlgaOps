"""Test deployment reproducibility and configuration consistency (WP-4A-7)."""

from __future__ import annotations

import re
import tomllib
from pathlib import Path

import pytest

from algaops_gateway.config import load_config, validate

ROOT = Path(__file__).resolve().parents[2]


def test_gateway_example_toml_validity():
    example_path = ROOT / "config" / "gateway.example.toml"
    assert example_path.is_file(), "gateway.example.toml must exist"
    data = tomllib.loads(example_path.read_text(encoding="utf-8"))
    assert data["api"]["host"] == "127.0.0.1", "API must default to loopback"
    assert data["mqtt"]["host"] == "127.0.0.1", "MQTT must default to loopback"
    assert data["pbr"]["enabled"] is False, "PBR must default to disabled"


def test_mosquitto_configuration_hardening():
    conf_path = ROOT.parent / "deploy" / "mosquitto" / "mosquitto.conf"
    assert conf_path.is_file()
    txt = conf_path.read_text(encoding="utf-8")
    assert "allow_anonymous false" in txt
    assert "password_file /etc/mosquitto/algaops/passwd" in txt
    assert "acl_file /etc/mosquitto/algaops/acl.conf" in txt
    assert re.search(r"^listener\s+1883", txt, re.MULTILINE) is not None
    assert "max_queued_messages 20000" in txt


def test_mosquitto_acl_per_node_isolation():
    acl_path = ROOT.parent / "deploy" / "mosquitto" / "acl.conf"
    assert acl_path.is_file()
    txt = acl_path.read_text(encoding="utf-8")
    assert "user algaops-gateway" in txt
    assert "topic read algaops/v1/#" in txt
    assert "pattern write algaops/v1/nodes/%u/telemetry" in txt
    assert "pattern write algaops/v1/nodes/%u/status" in txt


def test_systemd_service_hardening():
    svc_path = ROOT.parent / "deploy" / "systemd" / "algaops-gateway.service"
    assert svc_path.is_file()
    txt = svc_path.read_text(encoding="utf-8")
    assert "User=algaops" in txt
    assert "Group=algaops" in txt
    assert "WorkingDirectory=/var/lib/algaops" in txt
    assert "ProtectSystem=strict" in txt
    assert "ProtectHome=yes" in txt
    assert "ReadWritePaths=/var/lib/algaops" in txt
    assert "NoNewPrivileges=yes" in txt
    assert "Restart=on-failure" in txt


def test_chrony_orphan_configuration():
    chrony_path = ROOT.parent / "deploy" / "chrony" / "chrony.conf"
    assert chrony_path.is_file()
    txt = chrony_path.read_text(encoding="utf-8")
    assert "local stratum 10 orphan" in txt
    assert "makestep 1.0 3" in txt


def test_firmware_provisioning_template():
    sdk_ex = ROOT.parent / "firmware" / "esp32-sensor-node" / "sdkconfig.example"
    assert sdk_ex.is_file()
    txt = sdk_ex.read_text(encoding="utf-8")
    assert 'CONFIG_IDF_TARGET="esp32"' in txt
    assert "CONFIG_FREERTOS_HZ=1000" in txt
    assert "CONFIG_ESP_TASK_WDT_PANIC=y" in txt
    assert "CONFIG_ALGAOPS_NODE_ID=" in txt
    assert "CONFIG_ALGAOPS_MQTT_URI=" in txt
    assert "CONFIG_ALGAOPS_MQTT_USERNAME" in txt
    assert "CONFIG_ALGAOPS_I2C_SDA_GPIO=21" in txt
    assert "CONFIG_ALGAOPS_I2C_SCL_GPIO=22" in txt
    assert "CONFIG_ALGAOPS_ONEWIRE_GPIO=4" in txt

