# MQTT contract (v1)

Sensor nodes (ESP32 firmware or the simulator) publish; the gateway only
subscribes.  There is no command topic: nothing can be switched over MQTT.

| Topic | Direction | QoS | Retained | Payload schema |
|---|---|---|---|---|
| `algaops/v1/nodes/<node_id>/telemetry` | node -> gateway | 1 | no | [`telemetry_window.v1`](../schemas/telemetry_window.v1.schema.json) |
| `algaops/v1/nodes/<node_id>/status` | node -> gateway (and last will) | 1 | yes | [`node_status.v1`](../schemas/node_status.v1.schema.json) |

`node_id`: `^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$`, identical in the topic and the
payload.  With the deployed Mosquitto ACL a node can only publish under its own
user name ([deploy/mosquitto/acl.conf](../deploy/mosquitto/acl.conf)).  MQTT 3.1.1, payload ≤ 8 KiB.

## Telemetry: one message per window

```json
{"schema":"algaops.telemetry_window.v1","node_id":"esp32-a1b2c3d4e5f6","boot_id":3735928559,
 "seq":42,"t_start_ms":1793000059000,"t_end_ms":1793000060280,"window_ms":1280,
 "time_synced":true,"sample_rate_hz":800.0,"samples_expected":1024,"samples_valid":1024,
 "vib":{"rms_g":0.42000,"rms_axis_g":[0.24250,0.24250,0.24250],"p2p_g":[1.21000,1.21000,1.21000]},
 "temp_c":31.0625,"self_test":"pass","faults":[],"fw":"2.0.0","source":"hardware",
 "health":{"i2c_errors":0,"ow_crc_errors":0,"fifo_overruns":0,"queue_depth":3,
           "dropped_windows":0,"uptime_s":61,"reconnects":0,"rssi_dbm":-58}}
```

| Field | Meaning |
|---|---|
| `boot_id` | random per boot; a new value means the node restarted |
| `seq` | 1, 2, 3 … per boot, wraps after 2^32-1 (compared with RFC 1982 arithmetic) |
| `t_end_ms` | window end, node clock, Unix ms; `time_synced` false until SNTP has set the clock |
| `samples_expected` / `samples_valid` | lost samples are never filled in |
| `vib.rms_g` | AC RMS of the acceleration vector (gravity removed), g; `null` if no valid data |
| `temp_c`, `current_a` | `null` when the reading is invalid; `current_a` absent without a CT |
| `self_test` | ADXL345 data-sheet self-test: `pass`, `fail`, `not_run` |
| `faults` | node-side sensor faults: `ACCEL_BUS`, `ACCEL_OVERRUN`, `ACCEL_MISSING`, `TEMP_CRC`, `TEMP_MISSING`, `TEMP_POR`, `CURRENT_ADC`, `CURRENT_CLIP` |
| `source` | `hardware`, `simulator` or `replay`; shown as LIVE HARDWARE vs DEMO / SYNTHETIC |

Unknown fields are rejected (ground-truth labels can never ride along).

## Delivery semantics

* At least once: the node removes a window from its buffer only after the
  broker's PUBACK and resends identical bytes on retry.
* The gateway subscribes with a fixed client id and a persistent session, so
  the broker queues QoS 1 messages while the gateway restarts; it acknowledges
  a message only after the window, decision and audit event are committed.
* Exact redelivery (same node, boot, seq and payload bytes): dropped and
  logged.  Same seq with different content: `DUPLICATE_SEQ` -> CHECK_SENSOR.
* Gap in `seq`: `MISSING_PACKET` (informational; lost windows are never
  treated as valid).  Older seq: `OUT_OF_ORDER_SEQ` -> CHECK_SENSOR.
* Received more than 10 s after `t_end_ms`: `STALE`; more than 2 s in the
  future: `FUTURE_TIMESTAMP`; no window for 15 s: `STREAM_TIMEOUT` -> CHECK_SENSOR
  (and a CHECK_SENSOR alert).  Thresholds: `gateway/config/policy.default.toml`.
* Invalid JSON or schema violations on a node topic: rejected, logged in the
  ingest log and audited, and the node gets a CHECK_SENSOR (`MALFORMED`) decision.

## Status

Retained `online` message after each (re)connection and every 60 s, with boot
id, firmware, uptime, RSSI, buffer depth, dropped windows, self-test result,
sensor flags and reset reason.  The last will publishes `offline`.  Status is
informational: it never changes a diagnostic decision.

## Versioning

A breaking payload change gets a new schema id and topic prefix
(`algaops/v2/...`); v1 stays readable during migration.
