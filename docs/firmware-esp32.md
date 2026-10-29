# ESP32 sensor-node firmware

`firmware/esp32-sensor-node` — ESP-IDF ≥ 5.2, target `esp32` (e.g.
ESP32-DevKitC).  The node measures and reports; it has no actuator output and
subscribes to nothing.

## What it does

| Task | Core / priority | Work |
|---|---|---|
| accel | 1 / 10 | drains the ADXL345 FIFO every 10 ms (800 Hz, full resolution), closes a window every 1024 samples (1.28 s); watched by the task watchdog |
| temp | 1 / 6 | one DS18B20 conversion per second (12 bit), CRC and power-on value checks |
| ct (optional) | 1 / 9 | 1 kHz ADC sampling of a clamp-on CT, AC RMS per window |
| publish | 0 / 4 | QoS 1 publishing; a window leaves the RAM buffer only after its PUBACK; retained status every 60 s; last will "offline" |

* Features (per-axis AC RMS, peak-to-peak, vector RMS) use the C core's
  `features.c` (component `algaops_features`), the same code as the gateway.
* Windows carry `boot_id` (random per boot), `seq` (1, 2, … per boot), sample
  counts, `self_test` and node fault flags; see [mqtt-contract.md](mqtt-contract.md).
* Times are kept on the monotonic clock and converted when published: windows
  buffered before SNTP has set the clock still get correct Unix times.
  Telemetry waits up to `ALGAOPS_SYNC_WAIT_S` for the first sync.
* Bounded buffer (`ALGAOPS_BUFFER_WINDOWS`, default 256 ≈ 5.5 min): when full
  the oldest window is dropped and the gateway sees the gap.
* Sensor failures never stop the window cadence: an absent accelerometer still
  yields windows with `vib.rms_g: null` and `ACCEL_MISSING`, so the gateway
  shows CHECK_SENSOR instead of a silent node.

## Build and flash

```bash
. $HOME/esp/esp-idf/export.sh                       # ESP-IDF >= 5.2
cd firmware/esp32-sensor-node
idf.py set-target esp32
idf.py menuconfig        # "AlgaOps sensor node": Wi-Fi, MQTT URI/user/password, SNTP server, pins
idf.py build flash monitor
```

The IDF component manager fetches `espressif/onewire_bus` on the first build.
Credentials end up in the local `sdkconfig`, which is git-ignored.  Use the
node id as MQTT user name (`ALGAOPS_MQTT_USERNAME`) to match the broker ACL.

Main settings (`main/Kconfig.projbuild`, `components/current_sensor/Kconfig`):

| Key | Default | |
|---|---|---|
| `ALGAOPS_NODE_ID` | `esp32-<MAC>` | MQTT client id and topic |
| `ALGAOPS_MQTT_URI` | `mqtt://192.168.1.10:1883` | placeholder: the gateway's address |
| `ALGAOPS_SNTP_SERVER` | `192.168.1.10` | placeholder: chrony on the gateway |
| `ALGAOPS_WINDOW_MS` / `ALGAOPS_ACCEL_RATE_HZ` | 1280 / 800 | 1024 samples per window |
| `ALGAOPS_ACCEL_RANGE_G` | 8 | full resolution keeps 3.9 mg/LSB on every range |
| `ALGAOPS_ACCEL_SUPPLY_MV` | 3300 | scales the data-sheet self-test limits |
| `ALGAOPS_I2C_SDA_GPIO` / `SCL` / `ONEWIRE_GPIO` | 21 / 22 / 4 | see [hardware/esp32-pinout.csv](../hardware/esp32-pinout.csv) |
| `ALGAOPS_CURRENT_ENABLE` | n | optional CT on an ADC1 pin (GPIO 34) |
| `ALGAOPS_BUFFER_WINDOWS` | 256 | RAM buffer while offline |

## Tests

* **Host tests** (no hardware, run in CI and by `scripts/test_all.*`):
  register and scratchpad decoding, Dallas CRC-8 (Maxim AN27 vector), data-sheet
  self-test limits, window assembly and overrun accounting, CT RMS, health
  escalation, ring buffer, sequence arithmetic and the JSON payload.
  `gateway/tests/unit/test_firmware_payload_contract.py` feeds the firmware's
  JSON through the published schemas and the gateway's strict parser.
* **On-target tests** (`test_app/`, Unity): ADXL345 identity, self-test, data
  rate without overrun and 1 g at rest; DS18B20 presence, CRC and stable
  readings.  Flash with `idf.py -C test_app build flash monitor`, pump stopped.

## Verification status

* Host tests: pass.  The ESP-IDF-specific sources (drivers, Wi-Fi, MQTT, SNTP,
  tasks) were compiled only against stand-in headers to catch syntax and type
  errors; they have **not** been built with ESP-IDF or run on an ESP32 in this
  project's development environment.  `.github/workflows/firmware.yml` builds
  both projects with `espressif/esp-idf-ci-action`; run it (or `idf.py build`)
  and the on-target tests before relying on the node.
* The signatures and struct fields used were checked against the published
  headers: ESP-IDF `release/v5.3` (`driver/i2c_master.h`, `esp_adc/adc_oneshot.h`,
  `esp_adc/adc_cali_scheme.h`, `esp_sntp.h`), ESP-MQTT `mqtt_client.h` and
  `espressif/onewire_bus` 1.1.2.  Sensor logic follows the ADXL345 data sheet
  (Rev. G) and the DS18B20 data sheet; check the revision of the purchased parts.

## First boot checklist

1. Serial log shows `ADXL345 at 0x53`, `self-test PASS`, `DS18B20 …, 12 bit`.
2. `clock set by SNTP`, then `connected to the broker`.
3. Dashboard: the node appears as LIVE HARDWARE, status online, `self_test pass`.
4. Capture baselines per mode on the real pump (Baselines page).  Until a STEADY
   baseline exists, STEADY windows are NOT EVALUATED unless an absolute fixed
   limit is exceeded.
