/*
 * test_window_policy.c — completeness ring and stream-liveness tests.
 */

#include "test_harness.h"
#include "../include/window_policy.h"

static WindowPolicyConfig cfg_ring(uint32_t n, double minc, uint32_t span)
{
    WindowPolicyConfig c = window_policy_default_config();
    c.completeness_window = n;
    c.min_completeness = minc;
    c.min_span = span;
    return c;
}

static int test_disabled_by_default(void)
{
    WindowPolicyConfig c = window_policy_default_config();
    WindowPolicyState s;
    window_policy_init(&s);
    ASSERT(window_policy_record(&s, &c, 5) < 0.0, "disabled ring reports -1");
    ASSERT(!window_policy_low_completeness(&s, &c), "disabled ring never flags");
    uint64_t silence;
    window_policy_touch(&s, 1000);
    ASSERT(!window_policy_tick(&s, &c, 999999, &silence), "timeout disabled by default");
    return 0;
}

static int test_completeness_math(void)
{
    WindowPolicyConfig c = cfg_ring(10, 0.8, 5);
    WindowPolicyState s;
    window_policy_init(&s);
    for (int i = 0; i < 4; i++) window_policy_record(&s, &c, 0);
    ASSERT_NEAR(window_policy_completeness(&s, &c), 1.0, 1e-12, "4/4");
    ASSERT_NEAR(window_policy_record(&s, &c, 2), 5.0 / 7.0, 1e-12, "5 arrived of 7 slots");
    ASSERT(s.span == 7, "span counts arrivals and missing slots");
    ASSERT(window_policy_low_completeness(&s, &c), "71% < 80% with span 7 >= 5");
    /* Fill with arrivals until the missing slots roll out of the ring. */
    for (int i = 0; i < 10; i++) window_policy_record(&s, &c, 0);
    ASSERT_NEAR(window_policy_completeness(&s, &c), 1.0, 1e-12, "losses rolled out");
    ASSERT(!window_policy_low_completeness(&s, &c), "recovered");
    return 0;
}

static int test_min_span_suppresses_early_judgement(void)
{
    WindowPolicyConfig c = cfg_ring(60, 0.8, 30);
    WindowPolicyState s;
    window_policy_init(&s);
    window_policy_record(&s, &c, 0);
    window_policy_record(&s, &c, 3);   /* 2 of 5 */
    ASSERT(!window_policy_low_completeness(&s, &c), "not judged before min_span slots");
    return 0;
}

static int test_huge_gap_resets_ring(void)
{
    WindowPolicyConfig c = cfg_ring(20, 0.8, 5);
    WindowPolicyState s;
    window_policy_init(&s);
    for (int i = 0; i < 20; i++) window_policy_record(&s, &c, 0);
    double comp = window_policy_record(&s, &c, 1000);
    ASSERT_NEAR(comp, 1.0 / 20.0, 1e-12, "only the new arrival survives a ring-sized gap");
    ASSERT(s.span == 20, "ring full after reset");
    return 0;
}

static int test_stream_timeout_once_per_silence(void)
{
    WindowPolicyConfig c = window_policy_default_config();
    c.stream_timeout_ms = 5000;
    WindowPolicyState s;
    window_policy_init(&s);
    uint64_t silence = 0;
    ASSERT(!window_policy_tick(&s, &c, 100000, &silence), "no data yet => no timeout");
    window_policy_touch(&s, 10000);
    ASSERT(!window_policy_tick(&s, &c, 14000, &silence), "4 s silence < timeout");
    ASSERT(window_policy_tick(&s, &c, 15500, &silence), "5.5 s silence => timeout");
    ASSERT(silence == 5500, "silence length reported");
    ASSERT(!window_policy_tick(&s, &c, 30000, &silence), "reported once per silence period");
    window_policy_touch(&s, 31000);
    ASSERT(!window_policy_tick(&s, &c, 32000, &silence), "fresh data clears the timeout");
    ASSERT(window_policy_tick(&s, &c, 40000, &silence), "next silence reported again");
    return 0;
}

int main(void)
{
    printf("=== Window Policy Tests ===\n");
    RUN_TEST(test_disabled_by_default);
    RUN_TEST(test_completeness_math);
    RUN_TEST(test_min_span_suppresses_early_judgement);
    RUN_TEST(test_huge_gap_resets_ring);
    RUN_TEST(test_stream_timeout_once_per_silence);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
