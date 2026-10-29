# Validation protocol (real hardware)

The repository's numbers come from **synthetic** scenarios.  Before a site
relies on CHECK EQUIPMENT, measure on its own equipment.  This protocol
describes how; acceptance values are proposals to agree with the plant owner.

## Rules

* Never induce faults on production pumps, blowers or a running PBR.  Equipment
  anomalies are created only on a separate test rig (e.g. a spare pump or fan).
* Every recording gets a metadata file following
  [`recording_session.v1`](../schemas/recording_session.v1.schema.json)
  (rig, sensors, reference instruments, operator, supervisor approval,
  conditions) in `data/hardware/`.  Ground truth stays in that file, never in
  telemetry.
* Keep the evidence bundle of every session; results without it don't count.

## Phases

| Phase | Where | Duration | Pass criterion (proposal) |
|---|---|---|---|
| 0 Bench | node on the desk | 1 h | on-target tests pass; 1 g ± 0.15 g at rest; DS18B20 within ±0.5 °C of a reference thermometer; CT within ±10 % of a clamp meter |
| 1 Installation | test rig, then real pump | 1 day | windows trusted (TrustScore 100) ≥ 99 % while running; no FIFO overruns; time synced |
| 2 Baselines | real pump | ≥ 3 days | OFF, STARTUP and STEADY baselines from trusted windows, covering day and night ambient temperature |
| 3 Normal operation | real pump | ≥ 14 days | CHECK EQUIPMENT alerts on normal operation ≤ 1 per week; every alert reviewed by a technician (feedback recorded) |
| 4 Sensor and transport faults | test rig (and node only on the real pump) | 1 day | each induced fault gives CHECK SENSOR within the expected latency and **never** CHECK EQUIPMENT |
| 5 Equipment anomalies | test rig only | 1 day | detection rate and latency per anomaly type measured and reported |

Phase 4 faults (safe): detach the accelerometer mount, unplug the DS18B20,
cover the probe with insulation (slow drift), switch the access point off for
30 s and 5 min, restart the gateway, power-cycle the node, send a malformed
payload with `mosquitto_pub` on a test node id.

Phase 5 anomalies (test rig only): small imbalance mass on a fan or impeller,
loosened mounting bolt, partially blocked inlet, elevated casing temperature
from a controlled heat source.  The rig owner approves each change.

## Metrics

Count per window and per alert, separately for each fault type:

* precision = TP / (TP + FP), recall = TP / (TP + FN), false-positive rate =
  FP / (FP + TN) for CHECK SENSOR on sensor/transport faults and CHECK
  EQUIPMENT on equipment faults;
* attribution errors: CHECK EQUIPMENT during a sensor or transport fault
  (target: zero);
* detection latency: fault start to first matching decision, and to the alert
  (an alert needs 3 consecutive windows, policy `alert.confirm_windows`);
* NOT EVALUATED share during normal running, false alerts per week.

Method comparison needs no extra runs: fixed limits, z-score and Mahalanobis
are computed for every trusted window and stored with the decision (dashboard
**Methods**, `decisions.jsonl` export), so they are compared on identical data.
Report the policy hash and engine version with every result.

## Report

Site, equipment, firmware and engine versions, policy hash, session ids,
evidence bundle digests, metrics per phase, all alerts with technician
feedback, deviations from this protocol.  State clearly what was not tested.
