# Hardware assembly (sensor node)

## Safety rules — read first

* **Low voltage only.**  The node runs from a certified 5 V USB power supply.
  Nothing in this project requires opening a pump, blower, socket, junction box
  or control cabinet.
* **Never touch, open or strip mains conductors**, and never work inside an
  enclosure that is or may be live.  If a sensor cannot be fitted without that,
  stop and ask a qualified electrician.
* The current sensor is optional.  If used, it must be a **split-core current
  transformer with voltage output** (built-in burden, e.g. 1 V at rated
  current), clamped around **one** insulated conductor by means of a
  commercially made, certified **AC line splitter** between plug and socket.
  Hard-wired equipment: the CT is installed by a qualified electrician.
  Never use a current-output CT without its burden: an open secondary can
  develop dangerous voltage.
* AlgaOps only measures.  Do not wire any node pin to a relay, contactor, motor
  driver or PBR controller.
* Keep the electronics out of splash zones and out of the culture: sensors sit
  on the outside of the pump or blower housing, never in the PBR medium.

## Parts

See [hardware/bom.csv](../hardware/bom.csv).  Core set: ESP32-DevKitC, ADXL345
breakout (3.3 V), waterproof DS18B20 probe (3-wire), 4.7 kΩ resistor, IP65
enclosure with cable glands, 5 V USB supply.  Gateway: Raspberry Pi 4/5.

## Wiring

Pin table: [hardware/esp32-pinout.csv](../hardware/esp32-pinout.csv).

```
ESP32 DevKitC          ADXL345 breakout (I2C)      DS18B20 (external power)
  3V3  ----------------- VCC, CS                     VDD (red)  ---- 3V3
  GND  ----------------- GND, SDO (address 0x53)     GND (black) --- GND
  GPIO21 (SDA) --------- SDA                         DQ (yellow) --- GPIO4
  GPIO22 (SCL) --------- SCL                         4.7 kΩ from DQ to 3V3

Optional CT (voltage output), bias network on the node board:
  3V3 --[10 kΩ]--+--[10 kΩ]-- GND        mid-point = 1.65 V, 10 µF from mid-point to GND
                 +-- CT lead A;  CT lead B -- GPIO34 (ADC1)
```

* CS high selects I2C on the ADXL345; SDO low gives address 0x53
  (`ALGAOPS_ADXL345_ALT_ADDR` for 0x1D).  Most breakouts include 4.7 kΩ
  I2C pull-ups; keep the I2C cable short (< 30 cm), twisted with GND.
* Use the DS18B20 with external power (three wires), not parasite power.
* Choose the CT rating close to, but above, the motor current (e.g. 5 A / 1 V
  for a 2 A pump): the ADC sees ±1.41 V peak around 1.65 V at rated current.
  Check the result against a clamp meter and set `ALGAOPS_CURRENT_MA_PER_VOLT`.

## Mounting

* **Accelerometer**: rigidly on the pump or blower housing near the bearing or
  motor end shield — stud, two-component epoxy or a strong magnet base on
  steel.  Not on plastic covers, guards or cable.  Note the orientation; keep
  it the same after maintenance (or recapture baselines).
* **Temperature probe**: pressed against the motor or casing surface with a
  metal clamp or pipe clip and a little thermal paste, insulated from air flow
  if possible.  It measures casing temperature, not fluid or culture temperature.
* **Node enclosure**: IP65, away from water and heat sources, cables through
  glands with strain relief and drip loops.  The ESP32 needs 2.4 GHz Wi-Fi
  coverage to the gateway's network.

## Commissioning

1. Flash and configure the firmware ([firmware-esp32.md](firmware-esp32.md)).
2. With the pump stopped, run the on-target tests (`test_app`): self-test pass,
   ~1 g at rest, plausible temperature.
3. Start the pump; confirm the node appears as LIVE HARDWARE with trusted data.
4. Capture OFF, STARTUP and STEADY baselines on the real pump (dashboard
   **Baselines**), then follow [validation-protocol.md](validation-protocol.md)
   before relying on CHECK EQUIPMENT.
