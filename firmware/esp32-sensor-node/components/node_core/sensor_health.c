/* sensor_health.c — see sensor_health.h */

#include "sensor_health.h"

#include <stddef.h>
#include <string.h>

static const char *const FAULT_NAMES[NODE_FAULT_COUNT] = {
    "ACCEL_BUS", "ACCEL_OVERRUN", "ACCEL_MISSING", "TEMP_CRC",
    "TEMP_MISSING", "TEMP_POR", "CURRENT_ADC", "CURRENT_CLIP",
};

const char *node_fault_name(int index)
{
    return (index >= 0 && index < NODE_FAULT_COUNT) ? FAULT_NAMES[index] : NULL;
}

const char *node_self_test_str(node_self_test_t s)
{
    switch (s) {
        case NODE_SELF_TEST_PASS: return "pass";
        case NODE_SELF_TEST_FAIL: return "fail";
        default:                  return "not_run";
    }
}

void sensor_health_init(sensor_health_t *h)
{
    if (h) memset(h, 0, sizeof(*h));
}

void sensor_health_accel_ok(sensor_health_t *h)
{
    if (!h) return;
    h->accel_failures = 0;
    h->sticky_faults &= ~(uint32_t)NODE_FAULT_ACCEL_MISSING;
}

void sensor_health_accel_bus_error(sensor_health_t *h)
{
    if (!h) return;
    h->i2c_errors++;
    h->accel_failures++;
    h->window_faults |= NODE_FAULT_ACCEL_BUS;
    if (h->accel_failures >= SENSOR_HEALTH_ACCEL_MISSING_AFTER) {
        h->sticky_faults |= NODE_FAULT_ACCEL_MISSING;
    }
}

void sensor_health_accel_absent(sensor_health_t *h)
{
    if (!h) return;
    h->accel_failures = SENSOR_HEALTH_ACCEL_MISSING_AFTER;
    h->sticky_faults |= NODE_FAULT_ACCEL_MISSING;
}

void sensor_health_accel_overrun(sensor_health_t *h)
{
    if (!h) return;
    h->fifo_overruns++;
    h->window_faults |= NODE_FAULT_ACCEL_OVERRUN;
}

void sensor_health_self_test(sensor_health_t *h, bool passed)
{
    if (h) h->self_test = passed ? NODE_SELF_TEST_PASS : NODE_SELF_TEST_FAIL;
}

void sensor_health_temp_ok(sensor_health_t *h)
{
    if (!h) return;
    h->temp_failures = 0;
    h->sticky_faults &= ~(uint32_t)NODE_FAULT_TEMP_MISSING;
}

static void temp_failure(sensor_health_t *h)
{
    h->temp_failures++;
    if (h->temp_failures >= SENSOR_HEALTH_TEMP_MISSING_AFTER) {
        h->sticky_faults |= NODE_FAULT_TEMP_MISSING;
    }
}

void sensor_health_temp_crc_error(sensor_health_t *h)
{
    if (!h) return;
    h->ow_crc_errors++;
    h->window_faults |= NODE_FAULT_TEMP_CRC;
    temp_failure(h);
}

void sensor_health_temp_absent(sensor_health_t *h)
{
    if (!h) return;
    h->window_faults |= NODE_FAULT_TEMP_MISSING;
    temp_failure(h);
}

void sensor_health_temp_power_on_value(sensor_health_t *h)
{
    if (!h) return;
    h->window_faults |= NODE_FAULT_TEMP_POR;
    temp_failure(h);
}

void sensor_health_current_adc_error(sensor_health_t *h)
{
    if (h) h->window_faults |= NODE_FAULT_CURRENT_ADC;
}

void sensor_health_current_clipped(sensor_health_t *h)
{
    if (h) h->window_faults |= NODE_FAULT_CURRENT_CLIP;
}

uint32_t sensor_health_take_window_faults(sensor_health_t *h)
{
    if (!h) return 0;
    uint32_t f = (h->window_faults | h->sticky_faults) & NODE_FAULT_ALL;
    h->window_faults = 0;
    return f;
}

bool sensor_health_accel_usable(const sensor_health_t *h)
{
    return h && !(h->sticky_faults & NODE_FAULT_ACCEL_MISSING);
}

bool sensor_health_temp_usable(const sensor_health_t *h)
{
    return h && !(h->sticky_faults & NODE_FAULT_TEMP_MISSING);
}

int sensor_health_fault_names(uint32_t faults, const char **names, int max_names)
{
    int n = 0;
    for (int i = 0; i < NODE_FAULT_COUNT && n < max_names; i++) {
        if (faults & (1u << i)) names[n++] = FAULT_NAMES[i];
    }
    return n;
}
