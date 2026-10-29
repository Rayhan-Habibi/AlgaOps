/*
 * sensor_health.h — sensor faults of the current window plus lifetime
 * counters for the "health" and status fields.
 *
 * Faults raised during a window are reported with that window and cleared
 * when it is published.  Persistent conditions (an absent accelerometer, a
 * silent 1-Wire bus) are re-raised for every window while they last, so the
 * gateway never treats a window from a failing sensor as trusted.
 */

#ifndef SENSOR_HEALTH_H
#define SENSOR_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "sensor_hal.h"

/* Consecutive failures before a sensor is declared missing. */
#define SENSOR_HEALTH_ACCEL_MISSING_AFTER 3u
#define SENSOR_HEALTH_TEMP_MISSING_AFTER  2u

typedef struct {
    uint32_t window_faults;        /* NODE_FAULT_* of the window being assembled */
    uint32_t sticky_faults;        /* conditions re-raised for every window      */
    uint32_t i2c_errors;           /* lifetime counters                           */
    uint32_t ow_crc_errors;
    uint32_t fifo_overruns;
    uint32_t accel_failures;       /* consecutive                                 */
    uint32_t temp_failures;        /* consecutive                                 */
    node_self_test_t self_test;
} sensor_health_t;

void sensor_health_init(sensor_health_t *h);

void sensor_health_accel_ok(sensor_health_t *h);
void sensor_health_accel_bus_error(sensor_health_t *h);
void sensor_health_accel_absent(sensor_health_t *h);
void sensor_health_accel_overrun(sensor_health_t *h);
void sensor_health_self_test(sensor_health_t *h, bool passed);

void sensor_health_temp_ok(sensor_health_t *h);
void sensor_health_temp_crc_error(sensor_health_t *h);
void sensor_health_temp_absent(sensor_health_t *h);
void sensor_health_temp_power_on_value(sensor_health_t *h);

void sensor_health_current_adc_error(sensor_health_t *h);
void sensor_health_current_clipped(sensor_health_t *h);

/** Faults of the finished window (window + sticky); clears the window faults. */
uint32_t sensor_health_take_window_faults(sensor_health_t *h);

bool sensor_health_accel_usable(const sensor_health_t *h);
bool sensor_health_temp_usable(const sensor_health_t *h);

/** Write the contract names of @p faults (bit order) to @p names; returns the count. */
int sensor_health_fault_names(uint32_t faults, const char **names, int max_names);

#endif /* SENSOR_HEALTH_H */
