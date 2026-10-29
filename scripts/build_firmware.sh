#!/usr/bin/env bash
# Build the ESP32 sensor-node firmware with ESP-IDF (>= 5.2), e.g. after
#   . $HOME/esp/esp-idf/export.sh
# Without ESP-IDF only the host tests of the node logic are built and run.
#   scripts/build_firmware.sh            firmware (+ HIL test app with --test-app)
#   scripts/build_firmware.sh --test-app
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$ROOT/firmware/esp32-sensor-node"

if command -v idf.py >/dev/null 2>&1; then
    echo "==> ESP-IDF $(idf.py --version 2>/dev/null | tail -1)"
    if [[ ! -f "$FW/sdkconfig" ]]; then
        idf.py -C "$FW" set-target esp32
    fi
    idf.py -C "$FW" build
    if [[ "${1:-}" == "--test-app" ]]; then
        [[ -f "$FW/test_app/sdkconfig" ]] || idf.py -C "$FW/test_app" set-target esp32
        idf.py -C "$FW/test_app" build
    fi
    echo "flash with: idf.py -C $FW -p <PORT> flash monitor"
else
    echo "idf.py not found: building and running the host tests only" >&2
    CMAKE="$(command -v cmake || echo "$ROOT/.venv/bin/cmake")"
    "$CMAKE" -S "$FW/host_test" -B "$FW/host_test/build" -DNODE_HOST_WERROR=ON
    "$CMAKE" --build "$FW/host_test/build" -j
    ctest --test-dir "$FW/host_test/build" --output-on-failure
fi
