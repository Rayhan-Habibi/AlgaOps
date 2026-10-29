#!/usr/bin/env bash
# Native install of the AlgaOps gateway on Raspberry Pi OS (Bookworm, 64-bit).
# Run from the repository root:
#   sudo deploy/raspberry-pi/install.sh                       # packages from PyPI
#   sudo deploy/raspberry-pi/install.sh --wheelhouse DIR      # offline (build_wheelhouse.sh)
#
# Result: Mosquitto with password auth + ACL, chrony serving the node subnet,
# the C core in /opt/algaops/lib, the gateway in /opt/algaops/venv, config in
# /etc/algaops, data in /var/lib/algaops and a hardened systemd service.
# Re-running is safe: existing configuration and secrets are kept.
# Written for, but not yet run on, a physical Raspberry Pi.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WHEELS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --wheelhouse) WHEELS="$(cd "$2" && pwd)"; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }
step() { printf '\033[36m==> %s\033[0m\n' "$*"; }

step "System packages"
apt-get update
apt-get install -y --no-install-recommends python3-venv python3-dev build-essential cmake \
    mosquitto mosquitto-clients chrony

step "Service user and directories"
id algaops >/dev/null 2>&1 || useradd --system --home-dir /var/lib/algaops --shell /usr/sbin/nologin algaops
install -d -o algaops -g algaops -m 0750 /var/lib/algaops /var/lib/algaops/exports
install -d -m 0755 /opt/algaops /opt/algaops/lib /opt/algaops/dashboard /opt/algaops/docs
install -d -o root -g algaops -m 0750 /etc/algaops

step "C core: build and test"
BUILD="$(mktemp -d)"
cmake -S "$REPO/algaops-assurance" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DALGAOPS_WERROR=ON
cmake --build "$BUILD" -j"$(nproc)"
ctest --test-dir "$BUILD" --output-on-failure
install -m 0644 "$BUILD/libalgaops.so" /opt/algaops/lib/libalgaops.so
install -m 0755 "$BUILD/algaops" /usr/local/bin/algaops
rm -rf "$BUILD"

step "Gateway (Python virtual environment)"
[[ -x /opt/algaops/venv/bin/python ]] || python3 -m venv /opt/algaops/venv
RUNTIME_REQ="$(mktemp)"
sed -n '1,/^# Development and tests/p' "$REPO/gateway/requirements.lock" > "$RUNTIME_REQ"
if [[ -n "$WHEELS" ]]; then
    /opt/algaops/venv/bin/pip install --no-index --find-links "$WHEELS" -r "$RUNTIME_REQ" setuptools
    /opt/algaops/venv/bin/pip install --no-index --find-links "$WHEELS" --no-deps --no-build-isolation "$REPO/gateway"
else
    /opt/algaops/venv/bin/pip install -r "$RUNTIME_REQ"
    /opt/algaops/venv/bin/pip install --no-deps "$REPO/gateway"
fi
rm -f "$RUNTIME_REQ"

step "Dashboard and documentation"
if [[ -f "$REPO/dashboard/dist/index.html" ]]; then
    cp -r "$REPO/dashboard/dist/." /opt/algaops/dashboard/
else
    echo "dashboard/dist missing: build it on a PC (npm run build) and copy it here; the API works without it" >&2
fi
cp "$REPO"/docs/*.md /opt/algaops/docs/

step "Configuration and secrets"
install -m 0640 -o root -g algaops "$REPO/gateway/config/policy.default.toml" /etc/algaops/policy.default.toml
new_secret() { /opt/algaops/venv/bin/python -c 'import secrets; print(secrets.token_urlsafe(24))'; }
for f in api_token mqtt_password; do
    if [[ ! -f "/etc/algaops/$f" ]]; then
        (umask 027; new_secret > "/etc/algaops/$f")
        chgrp algaops "/etc/algaops/$f"
    fi
done
if [[ ! -f /etc/algaops/gateway.toml ]]; then
    cat > /etc/algaops/gateway.toml <<'EOF'
gateway_id = "gw-01"
site = "pbr-site"

[mqtt]
host = "127.0.0.1"
port = 1883
client_id = "algaops-gateway"
username = "algaops-gateway"
password_file = "/etc/algaops/mqtt_password"

[storage]
db_path = "/var/lib/algaops/algaops.db"
checkpoint_file = "/var/lib/algaops/checkpoints.jsonl"   # copy regularly to other media
export_dir = "/var/lib/algaops/exports"

[api]
host = "127.0.0.1"          # set the LAN address to serve the dashboard on the plant network
port = 8080
token_file = "/etc/algaops/api_token"
dashboard_dir = "/opt/algaops/dashboard"

[engine]
library_path = "/opt/algaops/lib/libalgaops.so"
policy_file = "/etc/algaops/policy.default.toml"

[logging]
level = "INFO"
EOF
    chown root:algaops /etc/algaops/gateway.toml
    chmod 0640 /etc/algaops/gateway.toml
fi

step "Mosquitto (password authentication, topic ACL)"
install -d -o root -g mosquitto -m 0750 /etc/mosquitto/algaops
install -m 0644 "$REPO/deploy/mosquitto/mosquitto.conf" /etc/mosquitto/conf.d/algaops.conf
install -m 0640 -o root -g mosquitto "$REPO/deploy/mosquitto/acl.conf" /etc/mosquitto/algaops/acl.conf
PASSWD=/etc/mosquitto/algaops/passwd
[[ -f "$PASSWD" ]] || install -m 0640 -o root -g mosquitto /dev/null "$PASSWD"
if ! grep -q '^algaops-gateway:' "$PASSWD"; then
    mosquitto_passwd -b "$PASSWD" algaops-gateway "$(cat /etc/algaops/mqtt_password)"
fi
chown root:mosquitto "$PASSWD"
chmod 0640 "$PASSWD"

step "chrony (time source for the nodes)"
install -d -m 0755 /etc/chrony/conf.d
install -m 0644 "$REPO/deploy/chrony/chrony.conf" /etc/chrony/conf.d/algaops.conf

step "systemd service"
install -m 0644 "$REPO/deploy/systemd/algaops-gateway.service" /etc/systemd/system/algaops-gateway.service
systemctl daemon-reload
systemctl restart chrony mosquitto
systemctl enable --now algaops-gateway

cat <<EOF

Installed.  Next steps:
  * adjust the node subnet in /etc/chrony/conf.d/algaops.conf, then: systemctl restart chrony
  * one MQTT user per sensor node (user name = node id):
      sudo mosquitto_passwd $PASSWD <node-id> && sudo systemctl reload mosquitto
  * check:  systemctl status algaops-gateway   and   curl -s http://127.0.0.1:8080/api/health
  * API token for write actions in the dashboard: /etc/algaops/api_token
EOF
