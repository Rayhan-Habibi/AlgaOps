#!/usr/bin/env bash
# Run every test suite that does not need hardware.
#   scripts/test_all.sh            everything except the browser tests
#   scripts/test_all.sh --quick    skip the ~2 min live e2e suite
#   scripts/test_all.sh --browser  also Playwright (ALGAOPS_BROWSER_CHANNEL=chromium
#                                  after `npx playwright install chromium`)
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$ROOT/.venv/bin/python"
CTEST="$(command -v ctest || echo "$ROOT/.venv/bin/ctest")"
QUICK=0; BROWSER=0
for a in "$@"; do
    case "$a" in --quick) QUICK=1 ;; --browser) BROWSER=1 ;; esac
done

declare -a NAMES=() RESULTS=()
stage() {
    local name="$1"; shift
    printf '\033[36m==> %s\033[0m\n' "$name"
    if "$@"; then RESULTS+=("PASS"); else RESULTS+=("FAIL"); fi
    NAMES+=("$name")
}

stage "C core (CTest)" "$CTEST" --test-dir "$ROOT/algaops-assurance/build" --output-on-failure
stage "Firmware host tests (CTest)" "$CTEST" --test-dir "$ROOT/firmware/esp32-sensor-node/host_test/build" --output-on-failure
if [[ $QUICK -eq 1 ]]; then
    stage "Python (without live e2e)" bash -c "cd '$ROOT' && '$PY' -m pytest -m 'not e2e'"
else
    stage "Python (gateway, simulator, tools, live e2e)" bash -c "cd '$ROOT' && '$PY' -m pytest"
fi
if command -v npm >/dev/null 2>&1; then
    stage "Dashboard unit tests (Vitest)" bash -c "cd '$ROOT/dashboard' && npm test --silent"
    if [[ $BROWSER -eq 1 ]]; then
        stage "Dashboard browser tests (Playwright)" bash -c "cd '$ROOT/dashboard' && npm run e2e --silent"
    fi
fi

echo
failed=0
for i in "${!NAMES[@]}"; do
    printf '%-55s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
    [[ "${RESULTS[$i]}" == PASS ]] || failed=1
done
exit $failed
