---
name: algaops-assurance
description: Work on the AlgaOps Assurance repository (trust-gated pump/blower diagnostics for microalgae PBRs) - C11 core, gateway, simulator, dashboard, ESP32 firmware. Use when changing decision logic, payload contracts, audit/evidence code or tests in this repo.
---

# AlgaOps Assurance

## Before changing anything

1. Decision logic lives only in `algaops-assurance/src` (C11).  Gateway, CLI and
   dashboard consume `AlgaDecision`; change the C engine, then the ctypes
   mirror in `gateway/algaops_gateway/core/libalgaops.py` (struct offsets are
   ABI-checked at load time; bump `ALGAOPS_ABI_VERSION` when a struct changes).
2. Payload contracts are `schemas/*.schema.json`.  Firmware
   (`components/node_core/telemetry_payload.c`), simulator
   (`algaops_sim/payload.py`) and gateway (`ingest/payload_schema.py`) must
   agree; `gateway/tests/unit/test_firmware_payload_contract.py` and
   `simulator/tests/test_payload_conformance.py` enforce it.
3. Policy values are in `gateway/config/policy.default.toml` (flat dotted keys,
   read by C and Python); the policy hash is recorded with every decision.

## Rules

- Untrusted -> `CHECK_SENSOR`, always.  Equipment methods (fixed limits,
  z-score, regularized Mahalanobis) only run on trusted windows; a transient
  with inconsistent sensors is `NOT_EVALUATED`, never `CHECK_EQUIPMENT`.
- No actuation, relays, shutdown or setpoints anywhere.
- Keep `DEMO / SYNTHETIC` labelling and keep labels out of payloads.
- Evidence: Ascon-Hash256 chain, canonical JSON, checkpoints in DB + external
  file.  State the limitation (full rewrite incl. checkpoints is undetectable
  without an independent anchor).
- Report only results that were actually run; hardware and ESP-IDF builds are
  not available in the development environment unless stated.

## Verify

`scripts/test_all.ps1` (or `make test`): CTest (core), firmware host tests,
pytest (gateway, simulator, tools, live e2e), vitest; Playwright with
`npm run e2e` in `dashboard/`.  Golden outcomes:
`algaops-assurance/tests/golden/scenario_outcomes.csv`.
