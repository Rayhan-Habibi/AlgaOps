/* Sensor faults per window, persistent "missing" conditions and contract names. */

#include <string.h>

#include "sensor_health.h"
#include "test_harness.h"

static int test_clean_start(void)
{
    sensor_health_t h;
    sensor_health_init(&h);
    ASSERT(sensor_health_take_window_faults(&h) == 0, "no faults");
    ASSERT(h.self_test == NODE_SELF_TEST_NOT_RUN, "self-test not run yet");
    ASSERT(sensor_health_accel_usable(&h) && sensor_health_temp_usable(&h), "usable");
    return 0;
}

static int test_accel_bus_errors_escalate_to_missing(void)
{
    sensor_health_t h;
    sensor_health_init(&h);
    sensor_health_accel_bus_error(&h);
    ASSERT(sensor_health_take_window_faults(&h) == NODE_FAULT_ACCEL_BUS, "one bus error");
    ASSERT(sensor_health_take_window_faults(&h) == 0, "window faults are cleared");
    sensor_health_accel_bus_error(&h);
    sensor_health_accel_bus_error(&h);
    uint32_t f = sensor_health_take_window_faults(&h);
    ASSERT(f == (NODE_FAULT_ACCEL_BUS | NODE_FAULT_ACCEL_MISSING), "third consecutive: missing");
    ASSERT(sensor_health_take_window_faults(&h) == NODE_FAULT_ACCEL_MISSING,
           "missing persists in later windows");
    ASSERT(!sensor_health_accel_usable(&h), "not usable");
    sensor_health_accel_ok(&h);
    ASSERT(sensor_health_take_window_faults(&h) == 0 && sensor_health_accel_usable(&h),
           "a good read clears it");
    ASSERT(h.i2c_errors == 3, "lifetime counter");
    return 0;
}

static int test_overrun_and_self_test(void)
{
    sensor_health_t h;
    sensor_health_init(&h);
    sensor_health_accel_overrun(&h);
    sensor_health_accel_overrun(&h);
    ASSERT(sensor_health_take_window_faults(&h) == NODE_FAULT_ACCEL_OVERRUN, "overrun");
    ASSERT(h.fifo_overruns == 2, "counted");
    sensor_health_self_test(&h, false);
    ASSERT(h.self_test == NODE_SELF_TEST_FAIL, "fail");
    ASSERT(strcmp(node_self_test_str(h.self_test), "fail") == 0, "payload string");
    sensor_health_self_test(&h, true);
    ASSERT(strcmp(node_self_test_str(h.self_test), "pass") == 0, "pass");
    ASSERT(strcmp(node_self_test_str(NODE_SELF_TEST_NOT_RUN), "not_run") == 0, "not_run");
    sensor_health_accel_absent(&h);
    ASSERT(sensor_health_take_window_faults(&h) == NODE_FAULT_ACCEL_MISSING, "absent at boot");
    return 0;
}

static int test_temperature_faults(void)
{
    sensor_health_t h;
    sensor_health_init(&h);
    sensor_health_temp_crc_error(&h);
    ASSERT(sensor_health_take_window_faults(&h) == NODE_FAULT_TEMP_CRC, "CRC");
    sensor_health_temp_power_on_value(&h);
    ASSERT(sensor_health_take_window_faults(&h) == (NODE_FAULT_TEMP_POR | NODE_FAULT_TEMP_MISSING),
           "second consecutive failure: missing");
    ASSERT(!sensor_health_temp_usable(&h), "not usable");
    sensor_health_temp_ok(&h);
    ASSERT(sensor_health_take_window_faults(&h) == 0 && sensor_health_temp_usable(&h), "recovered");
    ASSERT(h.ow_crc_errors == 1, "CRC counter");
    sensor_health_current_adc_error(&h);
    sensor_health_current_clipped(&h);
    ASSERT(sensor_health_take_window_faults(&h) ==
           (NODE_FAULT_CURRENT_ADC | NODE_FAULT_CURRENT_CLIP), "current faults");
    return 0;
}

static int test_names_follow_the_payload_contract(void)
{
    /* schemas/telemetry_window.v1.schema.json, properties.faults.items.enum */
    static const char *const expected[NODE_FAULT_COUNT] = {
        "ACCEL_BUS", "ACCEL_OVERRUN", "ACCEL_MISSING", "TEMP_CRC",
        "TEMP_MISSING", "TEMP_POR", "CURRENT_ADC", "CURRENT_CLIP",
    };
    for (int i = 0; i < NODE_FAULT_COUNT; i++) {
        ASSERT(strcmp(node_fault_name(i), expected[i]) == 0, "name of bit i");
    }
    ASSERT(node_fault_name(NODE_FAULT_COUNT) == NULL && node_fault_name(-1) == NULL, "bounds");
    const char *names[NODE_FAULT_COUNT];
    int n = sensor_health_fault_names(NODE_FAULT_TEMP_POR | NODE_FAULT_ACCEL_BUS, names,
                                      NODE_FAULT_COUNT);
    ASSERT(n == 2 && strcmp(names[0], "ACCEL_BUS") == 0 && strcmp(names[1], "TEMP_POR") == 0,
           "bit order");
    ASSERT(sensor_health_fault_names(NODE_FAULT_ALL, names, 3) == 3, "respects max_names");
    return 0;
}

int main(void)
{
    printf("sensor_health\n");
    RUN_TEST(test_clean_start);
    RUN_TEST(test_accel_bus_errors_escalate_to_missing);
    RUN_TEST(test_overrun_and_self_test);
    RUN_TEST(test_temperature_faults);
    RUN_TEST(test_names_follow_the_payload_contract);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
