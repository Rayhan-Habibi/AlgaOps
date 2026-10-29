# AlgaOps Assurance — notes for coding agents

Trust-gated edge diagnostics for the pumps and blowers of microalgae
photobioreactors: first decide whether the sensor data can be trusted, only
then judge the equipment.  Outcomes: `NORMAL`, `CHECK_SENSOR`,
`CHECK_EQUIPMENT`, `NOT_EVALUATED`.

## Invariants (do not weaken)

- Untrusted data is always `CHECK_SENSOR`; `CHECK_EQUIPMENT` only on trusted
  windows.  Never relax TrustGuard to produce more NORMAL or equipment results.
- Read-only towards the plant: no pump/blower control, relays, automatic
  shutdown or PBR setpoints; the gateway subscribes only, the node publishes only.
- One decision implementation: the C11 engine in `algaops-assurance/`.  The
  gateway calls it through ctypes (`gateway/algaops_gateway/core/`, ABI-checked);
  never re-implement decision logic in Python or TypeScript.
- Audit chain: Ascon-Hash256 (vendored reference code, KAT-tested).  Never
  hand-roll crypto, never claim tamper-proofing (it is tamper-evident).
- Synthetic data is labelled `DEMO / SYNTHETIC`; ground-truth labels never go
  into payloads.  Do not claim hardware, ESP-IDF or Raspberry Pi results that
  were not run.

## Layout

`algaops-assurance/` C11 core + CLI + CTest · `gateway/` MQTT ingest, SQLite,
audit, FastAPI · `simulator/` deterministic scenarios · `dashboard/` Vite/TS UI ·
`firmware/esp32-sensor-node/` ESP-IDF node (+ `host_test/`, `test_app/`) ·
`tools/verify_evidence.py` standalone verifier · `tests/e2e/` live vertical
slice · `schemas/` JSON contracts · `deploy/` Mosquitto, chrony, systemd, Docker.

## Commands (Windows, from the repo root; the venv provides cmake, ninja, zig)

```
.\scripts\build_all.ps1          # core, firmware host tests, dashboard, Python packages
.\scripts\test_all.ps1           # CTest, host tests, pytest (incl. e2e), vitest
.\scripts\run_demo.ps1           # dev broker + gateway + simulator, dashboard on :8080
```

Linux / Raspberry Pi: `make all`, `make test`, `scripts/run_demo.sh`.
The core DLL is locked while a gateway process runs: stop it before rebuilding.
pytest.ini already passes `-q`; don't add another (it hides the summary line).
