# AlgaOps Metric Contract Specification

**Document Version:** 1.1.0  
**Effective Stage:** Stage 4A (WP-4A-3)  
**Standard:** Evidence-Driven Engineering & Metric Integrity  

---

## 1. Overview and Intent

This specification establishes the authoritative mathematical definitions, data sources, numerators, denominators, and operational interpretations for all diagnostic, trust, and latency metrics in AlgaOps Edge Assurance.

Diagnostic evaluations are conducted across two distinct operational scopes:
1. **Window-Level Scope:** Evaluates individual telemetry windows (1.28 s each) against statistical and physical thresholds.
2. **Episode-Level Scope:** Evaluates contiguous operational fault events spanning one or more windows.

---

## 2. Core Detection Metrics

### 2.1 Episode Detection Rate (EDR)
- **Operational Meaning:** The fraction of injected blocking sensor/transport fault episodes that produced `CHECK_SENSOR` within the latency limit of their fault class (`DEC-001`).
- **Formula:**
  $$\text{EDR} = \frac{\sum_{e \in \text{Blocking}} \mathbb{I}(L_{\text{msg}}(e) \le L_{\max}(k_e))}{|\text{Blocking}|}$$
- **Numerator:** Episodes whose first correct decision came at most $L_{\max}$ messages after onset (`within_latency_limit` in `fault_events`).
- **Denominator:** Contiguous fault segments labelled `sensor_fault` or `transport_fault` (`total_episodes`). Equipment episodes are reported separately (`equipment_detected` / `equipment_episodes`); dropped packets have no received window and are covered by GLR (4.1).
- **Latency limits ($L_{\max}$, `EDR_MAX_LATENCY_MESSAGES`):** `stuck_vibration` 10, `stuck_temperature` 1000, every other blocking fault 0. The simulator's labels do not separate a frozen accelerometer (detected at once) from repeated features, so both use the limit of 10.
- **Units:** Dimensionless ratio in $[0.0, 1.0]$.

### 2.2 Raw Window Recall
- **Operational Meaning:** Unfiltered recall across all evaluated telemetry windows during failure periods.
- **Formula:**
  $$\text{Recall}_{\text{raw}} = \frac{\text{TP}_{\text{windows}}}{\text{TP}_{\text{windows}} + \text{FN}_{\text{windows}}}$$
- **Numerator:** $\text{TP}_{\text{windows}}$ (windows where true condition is faulty and diagnosed as `CHECK_SENSOR` or `CHECK_EQUIPMENT`).
- **Denominator:** Total faulty windows (`support = TP + FN`).
- **Units:** Dimensionless ratio in $[0.0, 1.0]$.
- **Policy Rule:** Never exclude warm-up, ramp-up, or initial filter latency windows. All initial unflagged windows must be counted as False Negatives ($\text{FN}$).

### 2.3 False Negative Windows (`fn_windows`)
- **Operational Meaning:** Count of fault windows that did not produce `CHECK_SENSOR`, including the windows before a detector has enough history.
- **Units:** Integer count of windows.
- **Example:** In the repeated-features episode of `stuck_vibration`, TrustGuard needs `trust.stuck_window_size = 10` identical windows; the last good window is the first of them, so detection comes at the ninth fault window and 8 FN windows occur before it.

---

## 3. False Alarm Metrics

### 3.1 Operator False Alert Rate (`fp_alert_8h`)
- **Operational Meaning:** Number of actionable alert incidents opened and presented to the human operator during 8 hours of normal operation.
- **Formula:**
  $$\text{fp\_alert\_8h} = \sum \text{alerts\_opened in } \texttt{normal\_8h\_replay}$$
- **Units:** Count of alert lifecycle incidents.
- **Target (`DEC-003`):** $0$ `CHECK_SENSOR` and at most $1$ `CHECK_EQUIPMENT` alert per 8 hours.
- **Without the replay:** `null`, not 0 (`evaluate` without `--full` does not run `normal_8h_replay`).

### 3.2 Raw Window Anomaly Noise (`fp_window_8h`)
- **Operational Meaning:** Number of individual telemetry windows flagged anomalous by classifiers during 8 hours of normal operation.
- **Formula:**
  $$\text{fp\_window\_8h} = \sum \mathbb{I}(\text{outcome} \in \{\texttt{CHECK\_SENSOR}, \texttt{CHECK\_EQUIPMENT}\}) \text{ in } \texttt{normal\_8h\_replay}$$
- **Units:** Count of windows. `null` without the replay, as above.
- **Operational Distinction:** Statistical 3-sigma thresholds produce Gaussian tail excursions (~0.17% in canonical baseline). These transient windows are debounced by the alert engine and do not open operator alerts.

---

## 4. Transport and Network Integrity Metrics

### 4.1 Gap Logging Rate (`glr`, `drop_gap_detection_recall`)
- **Operational Meaning:** Fraction of injected drop episodes whose lost sequence numbers are covered by one row of the SQLite `gaps` table (`DEC-001`).
- **Formula:**
  $$\text{GLR} = \frac{\text{drop episodes covered by a gap row}}{\text{injected drop episodes}}$$
- **Matching:** A row counts for a drop only if it has the same `boot_id` and its `from_seq`..`to_seq` covers every lost sequence number, so gap rows opened by reordering or delay cannot stand in for a drop.
- **Denominator:** Network fault specifications with `type == "drop"` (`dropped_packets` and `gaps_missing_samples` give the packet count).
- **Value in Baseline:** $3 / 3 = 1.0$ (6 packets lost, 6 reported missing).

### 4.2 Reorder Detection Rate (`reorder_detection_rate`)
- **Operational Meaning:** Fraction of injected reorder faults where a window of the swapped pair carries the TrustGuard flag `OUT_OF_ORDER_SEQ`.
- **Formula:**
  $$\text{Rate}_{\text{reorder}} = \frac{\text{reorder faults with the flag}}{\text{injected reorder faults}}$$
- **Value in Baseline:** $1 / 1 = 1.0$.

### 4.3 Delay Detection Rate (`delay_detection_rate`)
- **Operational Meaning:** Fraction of injected delay faults where a delayed window carries the TrustGuard flag `STALE`.
- **Formula:**
  $$\text{Rate}_{\text{delay}} = \frac{\text{delay faults with the flag}}{\text{injected delay faults}}$$
- **Value in Baseline:** $1 / 1 = 1.0$.

### 4.4 Total Gap Records Count (`total_gap_records`)
- **Operational Meaning:** Total number of gap rows inserted into SQLite `gaps` table.
- **Value in Baseline:** 5 gap records (3 from drop faults, 1 from packet reorder, 1 from delayed packets).

### 4.5 Gap-Record-to-Drop Ratio (`gap_record_to_drop_ratio`)
- **Interpretation Rule:** `transport.gap_record_to_drop_ratio` ($5 / 3 = 1.6667$ in the baseline) is a **ratio of database gap records to injected drop faults**, **NOT** detection recall. Earlier reports called this value `glr`.
- It exceeds $1.0$ because sequence jumps induced by out-of-order delivery and transport delays also insert records into `gaps`. It must not be cited as a recall percentage.

---

## 5. Latency Measurement Contract

$$\begin{aligned}
T_1 &= \text{network packet boundary timestamp } (\texttt{nw.received\_ms}) \\
T_2 &= \text{C engine decision completion timestamp } (\texttt{decided\_ms}) \\
T_3 &= \text{SQLite database transaction commit timestamp } (\texttt{committed\_ms}) \\
T_4 &= \text{SSE publication to subscriber queues timestamp } (\texttt{published\_ms}) \\
T_5 &= \text{Browser DOM render event timestamp } (\texttt{rendered\_ms})
\end{aligned}$$

| Metric | Formula | Scope |
|---|---|---|
| `core_decision_ms` | $T_2 - T_1$ | Gateway network boundary to C diagnostic output |
| `sqlite_commit_ms` | $T_3 - T_1$ | Gateway network boundary to completed SQLite commit |
| `commit_duration_ms` | Elapsed transaction time | Monotonic timer for SQLite transaction block alone |
| `sse_publish_ms` | $T_4 - T_1$ | Gateway network boundary to in-memory SSE queue push |
| `data-dom-latency-ms` | $T_5 - T_1$ | Gateway network boundary to browser DOM table insertion |

> [!NOTE]
> All host measurements must be labeled: `SYNTHETIC SCENARIOS (HOST EVALUATOR ONLY — NOT PHYSICAL HARDWARE)`. Raspberry Pi 4 hardware latency is unverified until physical bench testing is authorized.

