/* Window assembly (collected vs lost samples), shared features and the CT RMS. */

#include <math.h>
#include <stdint.h>

#include "adxl345_convert.h"
#include "sample_window.h"
#include "test_harness.h"

#define N 1024
#define PI 3.14159265358979323846

static int16_t g_buf[3 * N];
static int16_t g_in[3 * N];

static int test_init_rejects_bad_sizes(void)
{
    sample_window_t w;
    ASSERT(!sample_window_init(&w, g_buf, 0), "zero");
    ASSERT(!sample_window_init(&w, g_buf, 1), "one sample has no variance");
    ASSERT(!sample_window_init(&w, g_buf, FEATURES_MAX_SAMPLES + 1), "above the core limit");
    ASSERT(!sample_window_init(&w, NULL, 16), "no buffer");
    ASSERT(sample_window_init(&w, g_buf, N), "valid");
    return 0;
}

static int test_fills_exactly_and_returns_the_rest(void)
{
    sample_window_t w;
    sample_window_init(&w, g_buf, 100);
    for (int i = 0; i < 3 * 120; i++) g_in[i] = (int16_t)i;
    ASSERT(sample_window_add(&w, g_in, 32) == 32, "first batch");
    ASSERT(!sample_window_full(&w), "not yet full");
    ASSERT(sample_window_add(&w, &g_in[3 * 32], 88) == 68, "only the room left is taken");
    ASSERT(sample_window_full(&w) && w.samples_valid == 100, "full with 100 samples");
    ASSERT(g_buf[3 * 99 + 2] == g_in[3 * 99 + 2], "samples copied in order");
    ASSERT(sample_window_add(&w, g_in, 5) == 0, "full window takes nothing");
    sample_window_reset(&w);
    ASSERT(w.samples_valid == 0 && !w.overrun, "reset");
    return 0;
}

static int test_overrun_shortens_the_window(void)
{
    sample_window_t w;
    sample_window_init(&w, g_buf, 100);
    sample_window_add(&w, g_in, 50);
    sample_window_mark_overrun(&w, 0);              /* unknown count: at least one */
    ASSERT(w.overrun && w.samples_lost == 1, "one sample counted as lost");
    sample_window_mark_overrun(&w, 10);
    ASSERT(w.samples_lost == 11, "lost samples accumulate");
    ASSERT(sample_window_add(&w, g_in, 100) == 39, "room = expected - valid - lost");
    ASSERT(sample_window_full(&w) && w.samples_valid == 89, "never reported complete");
    return 0;
}

static int test_features_match_the_signal(void)
{
    /* 50 Hz sine of 100 counts on X at 800 Hz, gravity (256 counts) on Z. */
    sample_window_t w;
    sample_window_init(&w, g_buf, N);
    for (int i = 0; i < N; i++) {
        g_in[3 * i + 0] = (int16_t)lround(100.0 * sin(2.0 * PI * 50.0 * i / 800.0));
        g_in[3 * i + 1] = 0;
        g_in[3 * i + 2] = 256;
    }
    sample_window_add(&w, g_in, N);
    VibFeatures f;
    ASSERT(sample_window_features(&w, ADXL345_G_PER_LSB_FULL_RES, &f) == ALGAOPS_OK, "features");
    double expect_rms = 100.0 / sqrt(2.0) * ADXL345_G_PER_LSB_FULL_RES;
    ASSERT_NEAR(f.rms_g[0], expect_rms, 0.002, "X RMS = A/sqrt(2)");
    ASSERT_NEAR(f.rms_g[2], 0.0, 1e-12, "gravity removed from the RMS");
    ASSERT_NEAR(f.mean_g[2], 256 * ADXL345_G_PER_LSB_FULL_RES, 1e-9, "gravity is the mean");
    ASSERT_NEAR(f.p2p_g[0], 200 * ADXL345_G_PER_LSB_FULL_RES, 2 * ADXL345_G_PER_LSB_FULL_RES,
                "peak-to-peak");
    ASSERT_NEAR(f.rms_mag_g, expect_rms, 0.002, "vector RMS");
    ASSERT(!f.frozen, "moving signal");

    sample_window_t one;
    sample_window_init(&one, g_buf, N);
    sample_window_add(&one, g_in, 1);
    ASSERT(sample_window_features(&one, ADXL345_G_PER_LSB_FULL_RES, &f) != ALGAOPS_OK,
           "a single sample has no features");
    return 0;
}

static int test_frozen_sensor_is_visible(void)
{
    sample_window_t w;
    sample_window_init(&w, g_buf, 64);
    for (int i = 0; i < 64; i++) {
        g_in[3 * i] = 12;
        g_in[3 * i + 1] = -3;
        g_in[3 * i + 2] = 250;
    }
    sample_window_add(&w, g_in, 64);
    VibFeatures f;
    ASSERT(sample_window_features(&w, ADXL345_G_PER_LSB_FULL_RES, &f) == ALGAOPS_OK, "features");
    ASSERT(f.frozen && f.p2p_max_g == 0.0 && f.rms_mag_g == 0.0, "frozen output reported");
    return 0;
}

static int test_ct_rms_removes_the_bias(void)
{
    ac_rms_acc_t a;
    ac_rms_reset(&a);
    double rms = -1.0;
    ASSERT(!ac_rms_result(&a, &rms), "no samples");
    ac_rms_add(&a, 1650.0, false);
    ASSERT(!ac_rms_result(&a, &rms), "one sample");
    ac_rms_reset(&a);
    /* 50 Hz, 300 mV amplitude on a 1650 mV bias, 1 kHz for 1.28 s (64 cycles). */
    for (int i = 0; i < 1280; i++) {
        ac_rms_add(&a, 1650.0 + 300.0 * sin(2.0 * PI * 50.0 * i / 1000.0), false);
    }
    ASSERT(ac_rms_result(&a, &rms), "result");
    ASSERT_NEAR(rms, 300.0 / sqrt(2.0), 0.01, "AC RMS independent of the bias");
    ac_rms_add(&a, NAN, false);
    ASSERT(a.n == 1280, "non-finite samples are ignored");
    ac_rms_add(&a, 1650.0, true);
    ac_rms_error(&a);
    ASSERT(a.clipped == 1 && a.errors == 1, "clipping and errors counted");

    ac_rms_reset(&a);
    for (int i = 0; i < 100; i++) ac_rms_add(&a, 1650.0, false);
    ASSERT(ac_rms_result(&a, &rms) && rms == 0.0, "no current: exactly zero");
    return 0;
}

int main(void)
{
    printf("sample_window\n");
    RUN_TEST(test_init_rejects_bad_sizes);
    RUN_TEST(test_fills_exactly_and_returns_the_rest);
    RUN_TEST(test_overrun_shortens_the_window);
    RUN_TEST(test_features_match_the_signal);
    RUN_TEST(test_frozen_sensor_is_visible);
    RUN_TEST(test_ct_rms_removes_the_bias);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
