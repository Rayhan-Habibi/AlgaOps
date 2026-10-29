# Quickstart

Everything below runs on one PC with **synthetic data** (labelled DEMO /
SYNTHETIC everywhere).  Real sensors: [firmware-esp32.md](firmware-esp32.md),
[hardware-assembly.md](hardware-assembly.md), [deployment-raspberry-pi.md](deployment-raspberry-pi.md).

## Windows

Prerequisites: Python 3.11+ (`py` launcher), Node.js 20+ for the dashboard.
CMake, Ninja and a C compiler (zig cc) are installed into `.venv` by the script.

```powershell
.\scripts\build_all.ps1       # venv + packages, C core, firmware host tests, dashboard
.\scripts\test_all.ps1 -Quick # everything except the 2-minute live e2e suite
.\scripts\run_demo.ps1        # broker + gateway + three simulated pumps
```

The demo prints the dashboard address (http://127.0.0.1:8080, or the next free
port).  Ctrl+C stops it; data stays in `var\demo\`.

## Linux / Raspberry Pi OS

Prerequisites: `python3-venv`, `cmake`, a C compiler (`build-essential`), and
Node.js for the dashboard (or copy a prebuilt `dashboard/dist`).

```bash
make all          # scripts/build_all.sh
make test-quick   # or: make test (includes the live e2e slice)
make demo         # scripts/run_demo.sh
```

## What the demo shows

| Node | Scenario | Expected |
|---|---|---|
| sim-pump-01 | healthy pump | NORMAL (STARTUP judged by fixed limits, then STEADY by z-score) |
| sim-pump-02 | accelerometer output frozen, later repeated | CHECK SENSOR, never CHECK EQUIPMENT |
| sim-pump-03 | vibration rising to 3x on trusted data | CHECK EQUIPMENT once the deviation is significant |

When the scenarios end, the gateway reports a stream timeout (CHECK SENSOR) for
every node: silence is never shown as NORMAL.  The **Replay** page runs any
scenario offline against ground truth; **Methods** runs the labelled evaluation;
**Audit** verifies the hash chain; **Export** writes an evidence bundle.

## Without MQTT: the C command line

```bash
algaops-assurance/build/algaops --baseline algaops-assurance/data/baseline_normal.csv \
    --input algaops-assurance/data/scenario_equipment_anomaly.csv
# v2 policy (modes, methods, freshness) and a verifiable evidence directory:
algaops-assurance/build/algaops --config gateway/config/policy.default.toml \
    --baseline algaops-assurance/data/v2/baseline_modes.csv \
    --input algaops-assurance/data/v2/scenario_sensor_disagreement.csv --evidence out/ev
algaops-assurance/build/algaops --verify-evidence out/ev
python -I tools/verify_evidence.py out/ev            # independent Python verifier
```

(Windows: `algaops-assurance\build\algaops.exe`.)

## Labelled evaluation

```bash
PYTHONPATH=gateway:simulator .venv/bin/python -m algaops_gateway evaluate          # 17 scenarios
PYTHONPATH=gateway:simulator .venv/bin/python -m algaops_gateway evaluate --full   # + 8 h normal run
```

The report (precision, recall, false-positive rate, detection latency per
scenario) describes the logic on synthetic scenarios only, not field performance.
