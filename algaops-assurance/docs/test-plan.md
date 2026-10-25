# AlgaOps Assurance — Test Plan & Verification Matrix

> **Document Version:** 1.0.0  
> **Target Standard:** ISO/IEC 9899:2011 (C11)  
> **Testing Framework:** CTest & Custom Embedded-Compatible Assertion Harness

---

## 1. Testing Philosophy & Objectives

The primary objective of the AlgaOps Assurance test harness is to verify the core safety invariant:

> **Invariant:** A `CHECK_EQUIPMENT` outcome must **never** be emitted when input data fails integrity checks. Sensor anomalies must always result in `CHECK_SENSOR`.

All tests are completely standalone, deterministic, and execute without external dependencies, mock servers, or hardware emulators.

---

## 2. Test Suite Architecture

The test harness consists of three layers:

```
tests/
├── test_trustguard.c         Unit tests for Feature A (10 tests)
├── test_equipment_health.c   Unit tests for Feature B (8 tests)
└── test_integration.c        End-to-End Trust-Gating pipeline tests (6 tests)
```

Total automated test cases: **24 tests**.

---

## 3. Detailed Verification Matrix

### 3.1 Unit Test Suite: Feature A — TrustGuard (`test_trustguard.c`)

| Test ID | Test Function | Description / Verification Scenario | Expected Outcome |
| :--- | :--- | :--- | :--- |
| **TG-01** | `test_valid_monotonic` | Feeds sequential packets with advancing timestamps and valid physical readings. | Returns `ALGAOPS_OK`, `flags == INTEGRITY_OK`, `is_trusted == true`, `trust_score == 100`. |
| **TG-02** | `test_missing_sequence` | Injects gap (seq 1 to seq 4). Verifies missing count calculation ($4 - 2 = 2$). | `flags & INTEGRITY_MISSING_PACKET`, `is_trusted == true` (non-blocking), score reduced. |
| **TG-03** | `test_duplicate_sequence` | Feeds packet with identical sequence ID as previous record. | `flags & INTEGRITY_DUPLICATE_SEQ`, `is_trusted == false` (blocking). |
| **TG-04** | `test_out_of_order_sequence` | Feeds packet with sequence ID lower than previous accepted packet. | `flags & INTEGRITY_OUT_OF_ORDER_SEQ`, `is_trusted == false` (blocking). |
| **TG-05** | `test_non_monotonic_timestamp`| Injects backward timestamp ($t_2 < t_1$) and duplicate timestamp ($t_2 = t_1$). | `flags & INTEGRITY_BAD_TIMESTAMP`, `is_trusted == false` (blocking). |
| **TG-06** | `test_out_of_range` | Tests negative vibration ($< 0\,\text{g}$) and excessive temperature ($> 120\,^\circ\text{C}$). | `INTEGRITY_VIB_OUT_OF_RANGE` / `INTEGRITY_TEMP_OUT_OF_RANGE`, `is_trusted == false`. |
| **TG-07** | `test_stuck_vibration` | Injects $N=5$ consecutive identical vibration readings ($0.420000\,\text{g}$). | `flags & INTEGRITY_VIB_STUCK` on 5th packet, `is_trusted == false`. |
| **TG-08** | `test_non_finite` | Feeds `NAN` and `INFINITY` floating point values. | `flags & INTEGRITY_NON_FINITE`, `is_trusted == false`. |
| **TG-09** | `test_init_and_reset` | Tests clean state reset via `trustguard_reset()`, ensuring previous sequences do not leak. | New stream starts cleanly without false sequence/timestamp flags. |
| **TG-10** | `test_stuck_temperature` | Injects $N=5$ consecutive identical temperature readings ($28.000\,^\circ\text{C}$). | `flags & INTEGRITY_TEMP_STUCK`, `is_trusted == false`. |

---

### 3.2 Unit Test Suite: Feature B — Equipment Health (`test_equipment_health.c`)

| Test ID | Test Function | Description / Verification Scenario | Expected Outcome |
| :--- | :--- | :--- | :--- |
| **EH-01** | `test_normal_near_baseline` | Feeds sample matching baseline mean ($\mu_{vib} \approx 0.42\,\text{g}, \mu_{temp} \approx 28.15\,^\circ\text{C}$). | Returns `DIAG_NORMAL`, $z < 3.0$. |
| **EH-02** | `test_vibration_anomaly` | Feeds trusted sample with vibration elevated to $2.5\,\text{g}$ ($> 800\sigma$). | Returns `DIAG_CHECK_EQUIPMENT`, mentions vibration z-score. |
| **EH-03** | `test_temperature_anomaly` | Feeds trusted sample with temperature elevated to $60.0\,^\circ\text{C}$ ($> 300\sigma$). | Returns `DIAG_CHECK_EQUIPMENT`, mentions temperature z-score. |
| **EH-04** | `test_insufficient_baseline` | Attempts to initialize baseline from CSV with only 3 samples ($< 10$ required). | Returns `ALGAOPS_ERR_BASELINE`, logs informative error message. |
| **EH-05** | `test_near_zero_variance_baseline`| Baseline where all samples have identical vibration ($\sigma = 0$). | Safety floor `min_std_dev` ($10^{-6}$) applied, no division by zero or NaN. |
| **EH-06** | `test_deterministic_results` | Evaluates same input record 100 times in sequence. | Yields identical z-scores and outcome string every iteration. |
| **EH-07** | `test_trust_gating_suppresses_evaluation` | Feeds anomalous reading paired with `is_trusted = false`. | Evaluation suppressed: returns `DIAG_CHECK_SENSOR`, never `DIAG_CHECK_EQUIPMENT`. |
| **EH-08** | `test_null_pointer_robustness`| Passes `NULL` pointers to all public API functions. | Returns `ALGAOPS_ERR_NULL_PTR` without segfault. |

---

### 3.3 Integration Test Suite: Pipeline (`test_integration.c`)

| Test ID | Test Function | Pipeline Scenario | Expected Outcome |
| :--- | :--- | :--- | :--- |
| **INT-01** | `test_trusted_normal_produces_normal` | Clean, monotonic record near baseline fed through TG $\to$ EH. | `DIAG_NORMAL`. |
| **INT-02** | `test_trusted_anomaly_produces_check_equipment` | Clean sequence with high vibration ($2.0\,\text{g}$) fed through TG $\to$ EH. | `DIAG_CHECK_EQUIPMENT`. |
| **INT-03** | `test_untrusted_data_produces_check_sensor` | Bad timestamp or stuck sensor fed through TG $\to$ EH. | `DIAG_CHECK_SENSOR`. |
| **INT-04** | `test_suspicious_plus_blocking_yields_check_sensor` | Record has both severe vibration ($5.0\,\text{g}$) AND duplicate sequence ID. | **Core Invariant Verified:** `DIAG_CHECK_SENSOR`, never `DIAG_CHECK_EQUIPMENT`. |
| **INT-05** | `test_malformed_input_safe` | CSV containing corrupt tokens, empty lines, missing columns. | Reader skips bad lines with clear error; valid lines process safely. |
| **INT-06** | `test_missing_packet_not_blocking` | Sequence jump (e.g., seq 1 $\to$ seq 4) with healthy measurements. | `MISSING_PACKET` reported, but record trusted $\to$ `DIAG_NORMAL`. |

---

## 4. Synthetic Scenario Datasets (`data/`)

| File Name | Purpose | Fault Injected | Expected CLI Output |
| :--- | :--- | :--- | :--- |
| `baseline_normal.csv` | Baseline Statistics | None (30 clean samples at $\approx 1\,\text{s}$ interval) | Used by `--baseline` parameter to compute $\mu$ and $\sigma$. |
| `scenario_normal.csv` | Steady-State Verification | None (10 samples near baseline) | 100% `NORMAL`. |
| `scenario_missing_packet.csv` | Network Loss Simulation | Gaps: skipped seq 32-33 and seq 37 | Informational `MISSING_PACKET` flags; all records `NORMAL`. |
| `scenario_stuck_sensor.csv` | Hardware ADC Freeze | Vibration fixed at $0.42000\,\text{g}$ for 6 consecutive windows | Triggers `VIB_STUCK` $\to$ `CHECK_SENSOR` (suppressed). |
| `scenario_equipment_anomaly.csv`| Pump Degradation Simulation | Vibration climbs to $2.8\,\text{g}$, temperature climbs to $50\,^\circ\text{C}$ | Triggers `CHECK_EQUIPMENT` with explicit Z-score deviation breakdown. |

---

## 5. How to Run Verification

### 5.1 Using CTest (CMake)
```bash
cd build
ctest --output-on-failure
```

### 5.2 Direct Binary Execution
```bash
cd build
./test_trustguard
./test_equipment_health
./test_integration
```

All 24 test cases must output `PASS` with exit code `0`.
