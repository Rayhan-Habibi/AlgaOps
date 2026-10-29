#!/usr/bin/env bash
# Download the gateway's runtime wheels on a machine with internet access, for
# an offline install on the Raspberry Pi (install.sh --wheelhouse DIR).
#   deploy/raspberry-pi/build_wheelhouse.sh [OUT_DIR] [PYTHON_VERSION]
# Default target: Raspberry Pi OS Bookworm 64-bit = aarch64, CPython 3.11.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/wheelhouse}"
PYV="${2:-3.11}"
mkdir -p "$OUT"
REQ="$(mktemp)"
trap 'rm -f "$REQ"' EXIT
sed -n '1,/^# Development and tests/p' "$ROOT/gateway/requirements.lock" > "$REQ"
echo "setuptools>=68" >> "$REQ"     # build backend of the gateway package itself

python3 -m pip download --dest "$OUT" --only-binary=:all: --implementation cp \
    --python-version "$PYV" \
    --platform manylinux_2_28_aarch64 --platform manylinux_2_17_aarch64 \
    --platform manylinux2014_aarch64 --platform linux_aarch64 \
    -r "$REQ"
echo "wheels in $OUT ($(ls "$OUT" | wc -l) files); copy the directory to the Pi together with the repository"
