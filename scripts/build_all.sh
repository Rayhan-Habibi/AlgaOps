#!/usr/bin/env bash
# Build AlgaOps Assurance on Linux / Raspberry Pi OS: Python venv and pinned
# packages, the C11 core (system C compiler, or zig when none is installed),
# the firmware host tests and the dashboard (if npm is available).
# The ESP32 firmware itself needs ESP-IDF: scripts/build_firmware.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="$ROOT/.venv"
PY="$VENV/bin/python"
step() { printf '\033[36m==> %s\033[0m\n' "$*"; }

if [[ ! -x "$PY" ]]; then
    step "Creating the Python virtual environment (.venv)"
    python3 -m venv "$VENV"
fi
step "Python packages (pinned)"
"$PY" -m pip install --disable-pip-version-check -q \
    -r "$ROOT/gateway/requirements.lock" -r "$ROOT/simulator/requirements.lock"
"$PY" -m pip install --disable-pip-version-check -q --no-deps -e "$ROOT/gateway" -e "$ROOT/simulator"

CMAKE="$(command -v cmake || true)"
if [[ -z "$CMAKE" ]]; then
    "$PY" -m pip install -q "cmake==4.4.4" "ninja==1.13.2"
    CMAKE="$VENV/bin/cmake"
fi
TOOLCHAIN=(-DCMAKE_BUILD_TYPE=Release)
if ! command -v cc >/dev/null 2>&1; then
    step "No system C compiler: using zig cc from the venv"
    "$PY" -m pip install -q "ziglang==0.16.0"
    ZIG="$("$PY" -c 'import ziglang, os; print(os.path.join(os.path.dirname(ziglang.__file__), "zig"))')"
    printf '#!/bin/sh\nexec "%s" cc "$@"\n' "$ZIG" > "$VENV/bin/zcc"
    printf '#!/bin/sh\nexec "%s" ar "$@"\n' "$ZIG" > "$VENV/bin/zar"
    printf '#!/bin/sh\nexec "%s" ranlib "$@"\n' "$ZIG" > "$VENV/bin/zranlib"
    chmod +x "$VENV/bin/zcc" "$VENV/bin/zar" "$VENV/bin/zranlib"
    TOOLCHAIN+=(-DCMAKE_C_COMPILER="$VENV/bin/zcc" -DCMAKE_AR="$VENV/bin/zar"
                -DCMAKE_RANLIB="$VENV/bin/zranlib")
fi

step "C11 core (static + shared library, CLI, tests)"
"$CMAKE" -S "$ROOT/algaops-assurance" -B "$ROOT/algaops-assurance/build" -DALGAOPS_WERROR=ON "${TOOLCHAIN[@]}"
"$CMAKE" --build "$ROOT/algaops-assurance/build" -j

step "Firmware host tests (node logic without hardware)"
HT="$ROOT/firmware/esp32-sensor-node/host_test"
"$CMAKE" -S "$HT" -B "$HT/build" -DNODE_HOST_WERROR=ON "${TOOLCHAIN[@]}"
"$CMAKE" --build "$HT/build" -j

if [[ "${1:-}" != "--skip-dashboard" ]]; then
    if command -v npm >/dev/null 2>&1; then
        step "Dashboard (Vite build, served by the gateway)"
        (cd "$ROOT/dashboard" && { [[ -d node_modules ]] || npm ci --no-audit --no-fund; } && npm run build)
    else
        echo "npm not found: dashboard not built (copy a prebuilt dashboard/dist instead)" >&2
    fi
fi
step "Done"
