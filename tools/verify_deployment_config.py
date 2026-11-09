#!/usr/bin/env python3
"""AlgaOps Deployment & Configuration Reproducibility Validator (WP-4A-7).

Inspects deployment scripts, systemd units, broker configurations,
gateway templates, and firmware provisioning templates to verify
structural consistency, security boundaries, and reproducibility.

Usage:
    python tools/verify_deployment_config.py
"""

from __future__ import annotations

import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check(name: str, passed: bool, detail: str = "") -> bool:
    mark = "PASS" if passed else "FAIL"
    status_str = f"[{mark}] {name}"
    if detail:
        status_str += f": {detail}"
    print(status_str)
    return passed


def main() -> int:
    print("=" * 70)
    print("AlgaOps Edge Assurance — Deployment & Configuration Audit")
    print("=" * 70)
    all_ok = True

    # 1. Gateway Example TOML
    gw_example = ROOT / "gateway" / "config" / "gateway.example.toml"
    all_ok &= check("gateway.example.toml exists", gw_example.is_file())
    if gw_example.is_file():
        try:
            data = tomllib.loads(gw_example.read_text(encoding="utf-8"))
            all_ok &= check("gateway.example.toml valid TOML", True)
            all_ok &= check("gateway.example.toml loopback API default",
                            data.get("api", {}).get("host") == "127.0.0.1")
            all_ok &= check("gateway.example.toml loopback MQTT default",
                            data.get("mqtt", {}).get("host") == "127.0.0.1")
            all_ok &= check("gateway.example.toml PBR disabled by default",
                            data.get("pbr", {}).get("enabled") is False)
        except Exception as e:
            all_ok &= check("gateway.example.toml valid TOML", False, str(e))

    # 2. Mosquitto Broker Configuration
    mosq_conf = ROOT / "deploy" / "mosquitto" / "mosquitto.conf"
    all_ok &= check("mosquitto.conf exists", mosq_conf.is_file())
    if mosq_conf.is_file():
        txt = mosq_conf.read_text(encoding="utf-8")
        all_ok &= check("mosquitto allow_anonymous false", "allow_anonymous false" in txt)
        all_ok &= check("mosquitto password_file configured", "password_file /etc/mosquitto/algaops/passwd" in txt)
        all_ok &= check("mosquitto acl_file configured", "acl_file /etc/mosquitto/algaops/acl.conf" in txt)
        all_ok &= check("mosquitto listener 1883 present", re.search(r"^listener\s+1883", txt, re.MULTILINE) is not None)
        all_ok &= check("mosquitto queue >= 10000", "max_queued_messages 20000" in txt)

    # 3. Mosquitto ACL Configuration
    acl_conf = ROOT / "deploy" / "mosquitto" / "acl.conf"
    all_ok &= check("acl.conf exists", acl_conf.is_file())
    if acl_conf.is_file():
        txt = acl_conf.read_text(encoding="utf-8")
        all_ok &= check("acl gateway user has read on algaops/v1/#",
                        "user algaops-gateway" in txt and "topic read algaops/v1/#" in txt)
        all_ok &= check("acl telemetry pattern matches %u",
                        "pattern write algaops/v1/nodes/%u/telemetry" in txt)
        all_ok &= check("acl status pattern matches %u",
                        "pattern write algaops/v1/nodes/%u/status" in txt)

    # 4. Systemd Hardening & Service Definition
    systemd_svc = ROOT / "deploy" / "systemd" / "algaops-gateway.service"
    all_ok &= check("algaops-gateway.service exists", systemd_svc.is_file())
    if systemd_svc.is_file():
        txt = systemd_svc.read_text(encoding="utf-8")
        all_ok &= check("systemd unprivileged user algaops", "User=algaops" in txt and "Group=algaops" in txt)
        all_ok &= check("systemd ProtectSystem=strict", "ProtectSystem=strict" in txt)
        all_ok &= check("systemd ProtectHome=yes", "ProtectHome=yes" in txt)
        all_ok &= check("systemd ReadWritePaths restricted to /var/lib/algaops",
                        "ReadWritePaths=/var/lib/algaops" in txt)
        all_ok &= check("systemd NoNewPrivileges=yes", "NoNewPrivileges=yes" in txt)
        all_ok &= check("systemd Restart=on-failure", "Restart=on-failure" in txt)

    # 5. Chrony Time Configuration
    chrony_conf = ROOT / "deploy" / "chrony" / "chrony.conf"
    all_ok &= check("chrony.conf exists", chrony_conf.is_file())
    if chrony_conf.is_file():
        txt = chrony_conf.read_text(encoding="utf-8")
        all_ok &= check("chrony local stratum 10 orphan active", "local stratum 10 orphan" in txt)
        all_ok &= check("chrony makestep configured", "makestep 1.0 3" in txt)

    # 6. Raspberry Pi Install Script
    install_sh = ROOT / "deploy" / "raspberry-pi" / "install.sh"
    all_ok &= check("install.sh exists", install_sh.is_file())
    if install_sh.is_file():
        txt = install_sh.read_text(encoding="utf-8")
        all_ok &= check("install.sh requires root (sudo)", "[[ $EUID -eq 0 ]]" in txt)
        all_ok &= check("install.sh creates system user algaops", "useradd --system" in txt)
        all_ok &= check("install.sh sets up directory permissions 0750/0640", "0750" in txt and "0640" in txt)
        all_ok &= check("install.sh supports offline --wheelhouse", "--wheelhouse" in txt)

    # 7. ESP32 sdkconfig.example Provisioning Template
    sdk_ex = ROOT / "firmware" / "esp32-sensor-node" / "sdkconfig.example"
    all_ok &= check("sdkconfig.example exists", sdk_ex.is_file())
    if sdk_ex.is_file():
        txt = sdk_ex.read_text(encoding="utf-8")
        all_ok &= check("sdkconfig.example targets esp32", 'CONFIG_IDF_TARGET="esp32"' in txt)
        all_ok &= check("sdkconfig.example sets 1000 Hz FreeRTOS tick", "CONFIG_FREERTOS_HZ=1000" in txt)
        all_ok &= check("sdkconfig.example sets task watchdog panic", "CONFIG_ESP_TASK_WDT_PANIC=y" in txt)
        all_ok &= check("sdkconfig.example defines node_id placeholder", 'CONFIG_ALGAOPS_NODE_ID=' in txt)
        all_ok &= check("sdkconfig.example defines MQTT URI placeholder", 'CONFIG_ALGAOPS_MQTT_URI=' in txt)
        all_ok &= check("sdkconfig.example documents fallback MQTT username", "CONFIG_ALGAOPS_MQTT_USERNAME" in txt)
        all_ok &= check("sdkconfig.example pins SDA=21, SCL=22, OW=4",
                        "CONFIG_ALGAOPS_I2C_SDA_GPIO=21" in txt and
                        "CONFIG_ALGAOPS_I2C_SCL_GPIO=22" in txt and
                        "CONFIG_ALGAOPS_ONEWIRE_GPIO=4" in txt)

    # 8. Firmware node_app.c Verification
    node_app = ROOT / "firmware" / "esp32-sensor-node" / "main" / "node_app.c"
    if node_app.is_file():
        txt = node_app.read_text(encoding="utf-8")
        all_ok &= check("node_app.c has ALT_ADDR preprocessor guard", "#if CONFIG_ALGAOPS_ADXL345_ALT_ADDR" in txt)
        all_ok &= check("node_app.c has username fallback to node_id",
                        "const char *user = (CONFIG_ALGAOPS_MQTT_USERNAME[0] != '\\0')" in txt)

    # 9. Secret Sanitization & Node ID Conformance (WP-4A-12)
    node_id_pattern = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")
    if sdk_ex.is_file():
        sdk_txt = sdk_ex.read_text(encoding="utf-8")
        m = re.search(r'CONFIG_ALGAOPS_NODE_ID="([^"]+)"', sdk_txt)
        node_id_val = m.group(1) if m else ""
        all_ok &= check("sdkconfig.example node_id matches schema regex",
                        bool(node_id_pattern.match(node_id_val)), f"node_id='{node_id_val}'")
        all_ok &= check("sdkconfig.example uses placeholder wifi & mqtt passwords",
                        "replace_with_plant_wpa2_key" in sdk_txt and
                        "replace_with_node_mqtt_secret" in sdk_txt)

    if gw_example.is_file():
        gw_txt = gw_example.read_text(encoding="utf-8")
        all_ok &= check("gateway.example.toml uses placeholder secrets",
                        "secret_token_here" in gw_txt or "token_file" in gw_txt)

    print("=" * 70)
    print(f"Overall Result: {'ALL CHECKS PASSED' if all_ok else 'SOME CHECKS FAILED'}")
    print("=" * 70)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

