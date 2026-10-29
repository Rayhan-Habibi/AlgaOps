/*
 * current_sensor.h — optional motor current from a clamp-on current
 * transformer (voltage-output CT, biased to mid-supply) on an ESP32 ADC1 pin.
 *
 * A sampling task reads the ADC (esp_adc oneshot driver with calibration) at a
 * fixed rate and accumulates an exact AC RMS (mean removed, so the bias does
 * not matter).  The window code takes one RMS value per telemetry window.
 * The sensor only measures; nothing here can switch the pump.
 */

#ifndef CURRENT_SENSOR_H
#define CURRENT_SENSOR_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int      gpio;              /* ADC1 pin                                  */
    uint32_t ma_per_volt;       /* CT scale: mA primary per V RMS output    */
    uint32_t sample_hz;         /* 500..1000                                 */
} current_sensor_config_t;

typedef struct {
    bool     valid;             /* enough samples, no ADC failure            */
    float    amps_rms;
    uint32_t samples;
    uint32_t clipped;           /* samples at the ADC limits                 */
    uint32_t errors;            /* failed ADC reads                          */
} current_window_t;

/** Configure the ADC and start the sampling task (pinned to core 1). */
esp_err_t current_sensor_start(const current_sensor_config_t *cfg);

/** Close the running window and start the next one. */
void current_sensor_take_window(current_window_t *out);

#endif /* CURRENT_SENSOR_H */
