/*
 * test_operating_mode.c — OFF / STARTUP / STEADY state machine.
 */

#include "test_harness.h"
#include "../include/operating_mode.h"

static AlgaWindow win(uint64_t ts, double vib, int has_current, double cur)
{
    AlgaWindow w;
    memset(&w, 0, sizeof(w));
    w.timestamp_ms = ts;
    w.vibration_rms_g = vib;
    w.temperature_c = 30.0;
    if (has_current) {
        w.presence |= ALGA_HAS_CURRENT;
        w.current_a = cur;
    }
    return w;
}

static OperatingModeConfig enabled_cfg(void)
{
    OperatingModeConfig c = operating_mode_default_config();
    c.enabled = true;
    c.vib_on_g = 0.10;
    c.vib_off_g = 0.05;
    c.current_on_a = 0.5;
    c.current_off_a = 0.2;
    c.startup_ms = 10000;
    return c;
}

static int test_disabled_is_single_steady_mode(void)
{
    OperatingModeConfig c = operating_mode_default_config();
    OperatingModeState s;
    operating_mode_init(&s);
    AlgaWindow w = win(1000, 0.0, 0, 0.0);
    ASSERT(operating_mode_update(&s, &c, &w, NULL, NULL) == ALGA_MODE_STEADY,
           "v1 behaviour: always STEADY");
    return 0;
}

static int test_vibration_hysteresis_and_startup(void)
{
    OperatingModeConfig c = enabled_cfg();
    OperatingModeState s;
    operating_mode_init(&s);
    bool changed;
    uint64_t tim;
    AlgaWindow w = win(0, 0.02, 0, 0);
    ASSERT(operating_mode_update(&s, &c, &w, &changed, &tim) == ALGA_MODE_OFF, "idle => OFF");
    ASSERT(!changed, "first window is not a change");
    w = win(1000, 0.08, 0, 0);
    ASSERT(operating_mode_update(&s, &c, &w, &changed, &tim) == ALGA_MODE_OFF,
           "0.08 g below on-threshold stays OFF");
    w = win(2000, 0.40, 0, 0);
    ASSERT(operating_mode_update(&s, &c, &w, &changed, &tim) == ALGA_MODE_STARTUP, "switch-on");
    ASSERT(changed, "OFF -> STARTUP is a change");
    w = win(6000, 0.07, 0, 0);
    ASSERT(operating_mode_update(&s, &c, &w, &changed, &tim) == ALGA_MODE_STARTUP,
           "0.07 g above off-threshold keeps running (hysteresis)");
    w = win(12000, 0.42, 0, 0);
    ASSERT(operating_mode_update(&s, &c, &w, &changed, &tim) == ALGA_MODE_STEADY,
           "after startup_ms => STEADY");
    ASSERT(changed && tim == 0, "STARTUP -> STEADY change resets time in mode");
    w = win(15000, 0.43, 0, 0);
    operating_mode_update(&s, &c, &w, &changed, &tim);
    ASSERT(tim == 3000, "time in STEADY accumulates");
    w = win(16000, 0.03, 0, 0);
    ASSERT(operating_mode_update(&s, &c, &w, &changed, &tim) == ALGA_MODE_OFF, "switch-off");
    return 0;
}

static int test_current_preferred_when_present(void)
{
    OperatingModeConfig c = enabled_cfg();
    OperatingModeState s;
    operating_mode_init(&s);
    AlgaWindow w = win(0, 0.40, 1, 0.05);   /* vibration high but current off */
    ASSERT(operating_mode_update(&s, &c, &w, NULL, NULL) == ALGA_MODE_OFF,
           "current decides running state when present");
    w = win(1000, 0.01, 1, 1.9);
    ASSERT(operating_mode_update(&s, &c, &w, NULL, NULL) == ALGA_MODE_STARTUP,
           "current on => STARTUP even with low vibration");
    return 0;
}

static int test_labels(void)
{
    ASSERT(strcmp(operating_mode_str(ALGA_MODE_OFF), "OFF") == 0, "OFF");
    ASSERT(strcmp(operating_mode_str(ALGA_MODE_STARTUP), "STARTUP") == 0, "STARTUP");
    ASSERT(strcmp(operating_mode_str(ALGA_MODE_STEADY), "STEADY") == 0, "STEADY");
    ASSERT(strcmp(operating_mode_str(99), "UNKNOWN") == 0, "UNKNOWN");
    return 0;
}

int main(void)
{
    printf("=== Operating Mode Tests ===\n");
    RUN_TEST(test_disabled_is_single_steady_mode);
    RUN_TEST(test_vibration_hysteresis_and_startup);
    RUN_TEST(test_current_preferred_when_present);
    RUN_TEST(test_labels);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
