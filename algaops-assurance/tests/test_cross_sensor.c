/*
 * test_cross_sensor.c — current vs vibration running-state agreement.
 */

#include "test_harness.h"
#include "../include/cross_sensor.h"

static AlgaWindow win(uint64_t ts, double vib, double cur)
{
    AlgaWindow w;
    memset(&w, 0, sizeof(w));
    w.timestamp_ms = ts;
    w.vibration_rms_g = vib;
    w.temperature_c = 30.0;
    w.presence = ALGA_HAS_CURRENT;
    w.current_a = cur;
    return w;
}

static CrossSensorConfig enabled(uint32_t grace)
{
    CrossSensorConfig c = cross_sensor_default_config();
    c.enabled = true;
    c.grace_ms = grace;
    return c;
}

static int test_disabled_or_no_current(void)
{
    CrossSensorConfig c = cross_sensor_default_config();
    OperatingModeConfig m = operating_mode_default_config();
    CrossSensorState s;
    cross_sensor_init(&s);
    char e[256];
    AlgaWindow w = win(0, 0.01, 2.0);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_OK, "disabled");
    c.enabled = true;
    w.presence = 0;
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_OK, "no current");
    return 0;
}

static int test_startup_lag_is_pending_not_fault(void)
{
    CrossSensorConfig c = enabled(5000);
    OperatingModeConfig m = operating_mode_default_config();
    CrossSensorState s;
    cross_sensor_init(&s);
    char e[512];
    AlgaWindow w = win(0, 0.01, 0.02);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_OK, "both off");
    w = win(1280, 0.02, 2.8);   /* switch-on: current leads, vibration lags */
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_PENDING,
           "lag right after a current change is a transient");
    ASSERT(strstr(e, "transient") != NULL, "transient explained");
    w = win(2560, 0.55, 2.4);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_OK, "vibration followed");
    w = win(30000, 0.42, 0.02);  /* switch-off: coast-down */
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_PENDING, "coast-down");
    w = win(31280, 0.01, 0.02);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_OK, "stopped");
    return 0;
}

static int test_detached_accelerometer_flagged_immediately(void)
{
    CrossSensorConfig c = enabled(5000);
    OperatingModeConfig m = operating_mode_default_config();
    CrossSensorState s;
    cross_sensor_init(&s);
    char e[512];
    AlgaWindow w = win(0, 0.42, 1.9);
    cross_sensor_check(&s, &c, &m, &w, e, sizeof(e));
    w = win(60000, 0.42, 1.9);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_OK, "running, agree");
    /* vibration collapses while the current is unchanged for a minute */
    w = win(61280, 0.006, 1.9);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_FLAG,
           "no current transition explains the drop => flagged at once");
    ASSERT(strstr(e, "current") && strstr(e, "accelerometer"), "explanation names the evidence");
    return 0;
}

static int test_ct_clamp_off_flagged_after_grace(void)
{
    CrossSensorConfig c = enabled(5000);
    OperatingModeConfig m = operating_mode_default_config();
    CrossSensorState s;
    cross_sensor_init(&s);
    char e[512];
    AlgaWindow w = win(0, 0.42, 1.9);
    cross_sensor_check(&s, &c, &m, &w, e, sizeof(e));
    w = win(60000, 0.42, 0.0);   /* current reading drops, vibration continues */
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_PENDING,
           "looks like a stop at first");
    w = win(63000, 0.42, 0.0);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_PENDING, "within grace");
    w = win(65500, 0.43, 0.0);
    ASSERT(cross_sensor_check(&s, &c, &m, &w, e, sizeof(e)) == CROSS_FLAG,
           "vibration never followed the current => flagged");
    ASSERT(strstr(e, "vibration") && strstr(e, "CT clamp"), "explanation");
    return 0;
}

int main(void)
{
    printf("=== Cross-Sensor Consistency Tests ===\n");
    RUN_TEST(test_disabled_or_no_current);
    RUN_TEST(test_startup_lag_is_pending_not_fault);
    RUN_TEST(test_detached_accelerometer_flagged_immediately);
    RUN_TEST(test_ct_clamp_off_flagged_after_grace);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
