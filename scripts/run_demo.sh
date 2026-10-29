#!/usr/bin/env bash
# Local DEMO / SYNTHETIC run: development MQTT broker (or a running Mosquitto
# without authentication on the port), the gateway with its dashboard on
# http://127.0.0.1:8080 and three simulated pumps published in real time.
# Ctrl+C stops everything.  Data lands in var/demo/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
API_PORT="${API_PORT:-8080}"
MQTT_PORT="${MQTT_PORT:-1883}"
WORK="$ROOT/var/demo"
mkdir -p "$WORK"
export PYTHONPATH="$ROOT/gateway:$ROOT/simulator"
while (exec 3<>"/dev/tcp/127.0.0.1/$API_PORT") 2>/dev/null; do
    echo "port $API_PORT is in use by another program: trying $((API_PORT + 1))"
    API_PORT=$((API_PORT + 1))
done
[[ -f "$ROOT/dashboard/dist/index.html" ]] || echo "dashboard/dist missing: run scripts/build_all.sh (API works without it)" >&2

cat > "$WORK/gateway.toml" <<EOF
gateway_id = "demo-gw"
site = "lab (demo)"
[mqtt]
port = $MQTT_PORT
client_id = "algaops-gateway-demo"
[storage]
db_path = "algaops.db"
checkpoint_file = "checkpoints.jsonl"
export_dir = "exports"
[api]
port = $API_PORT
dashboard_dir = "$ROOT/dashboard/dist"
[engine]
policy_file = "$ROOT/gateway/config/policy.default.toml"
EOF

PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done   # SIGTERM: clean shutdown
    wait 2>/dev/null || true
    echo "stopped"
}
trap cleanup EXIT INT TERM

if (exec 3<>"/dev/tcp/127.0.0.1/$MQTT_PORT") 2>/dev/null; then
    echo "port $MQTT_PORT already has a broker: using it"
else
    "$PY" -m algaops_gateway devbroker --port "$MQTT_PORT" >"$WORK/broker.log" 2>&1 &
    PIDS+=($!)
    sleep 2
fi
"$PY" -m algaops_gateway --config "$WORK/gateway.toml" run >"$WORK/gateway.log" 2>&1 &
PIDS+=($!)
API="http://127.0.0.1:$API_PORT"
for _ in $(seq 60); do
    "$PY" -c "import urllib.request,sys; urllib.request.urlopen('$API/api/health', timeout=2)" 2>/dev/null && break
    sleep 1
done

declare -A NODES=([sim-pump-01]=normal_steady [sim-pump-02]=stuck_vibration
                  [sim-pump-03]=equipment_vibration_anomaly)
for node in "${!NODES[@]}"; do
    "$PY" - "$API" "$node" "$ROOT/algaops-assurance/data/v2/baseline_modes.csv" <<'PYEOF'
import json, sys, urllib.request
api, node, csv_path = sys.argv[1:4]
body = json.dumps({"node_id": node, "csv": open(csv_path, encoding="utf-8").read(),
                   "activate": True}).encode()
req = urllib.request.Request(f"{api}/api/baselines/import", data=body,
                             headers={"Content-Type": "application/json"})
urllib.request.urlopen(req, timeout=30).read()
PYEOF
    "$PY" -m algaops_sim publish --scenario "${NODES[$node]}" --node-id "$node" --port "$MQTT_PORT" \
        >"$WORK/sim-$node.log" 2>&1 &
    PIDS+=($!)
done
echo
echo "Dashboard: $API   (DEMO / SYNTHETIC data; Ctrl+C to stop)"
wait
