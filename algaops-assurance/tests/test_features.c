/*
 * test_features.c — RMS / peak-to-peak / spectral band features.
 */

#include "test_harness.h"
#include "../include/features.h"

#include <stdlib.h>

#define PI 3.14159265358979323846

static int16_t g_xyz[3 * 4096];

static int test_square_wave_rms_and_p2p(void)
{
    /* x: ±100 counts square wave around a DC offset of 256 (1 g), y const, z ±50 */
    for (uint32_t i = 0; i < 1000; i++) {
        g_xyz[3 * i + 0] = (int16_t)(256 + ((i & 1u) ? 100 : -100));
        g_xyz[3 * i + 1] = 7;
        g_xyz[3 * i + 2] = (int16_t)((i & 1u) ? 50 : -50);
    }
    VibFeatures f;
    double s = 0.0039;
    ASSERT(features_from_counts(g_xyz, 1000, s, &f) == ALGAOPS_OK, "compute");
    ASSERT_NEAR(f.mean_g[0], 256 * s, 1e-12, "DC (gravity) is the mean");
    ASSERT_NEAR(f.rms_g[0], 100 * s, 1e-12, "AC RMS removes DC");
    ASSERT_NEAR(f.rms_g[1], 0.0, 1e-12, "constant axis has zero AC RMS");
    ASSERT_NEAR(f.rms_g[2], 50 * s, 1e-12, "z RMS");
    ASSERT_NEAR(f.p2p_g[0], 200 * s, 1e-12, "x p2p");
    ASSERT_NEAR(f.p2p_g[1], 0.0, 1e-12, "y p2p");
    ASSERT_NEAR(f.rms_mag_g, sqrt(100.0 * 100.0 + 50.0 * 50.0) * s, 1e-12, "vector RMS");
    ASSERT_NEAR(f.p2p_max_g, 200 * s, 1e-12, "max p2p");
    ASSERT(!f.frozen, "signal is live");
    for (int a = 0; a < 3; a++) ASSERT(f.p2p_g[a] + 1e-12 >= f.rms_g[a], "p2p >= rms invariant");
    return 0;
}

static int test_frozen_detection(void)
{
    for (uint32_t i = 0; i < 64; i++) {
        g_xyz[3 * i] = 12; g_xyz[3 * i + 1] = -3; g_xyz[3 * i + 2] = 256;
    }
    VibFeatures f;
    ASSERT(features_from_counts(g_xyz, 64, 0.0039, &f) == ALGAOPS_OK, "compute");
    ASSERT(f.frozen, "identical samples on all axes => frozen");
    ASSERT(f.rms_mag_g == 0.0 && f.p2p_max_g == 0.0, "zero AC content");
    return 0;
}

static int test_extreme_values_exact(void)
{
    /* the full 8192-sample limit with alternating int16 extremes */
    static int16_t big[3 * FEATURES_MAX_SAMPLES];
    for (uint32_t i = 0; i < FEATURES_MAX_SAMPLES; i++) {
        int16_t v = (i & 1u) ? 32767 : -32768;
        big[3 * i] = v; big[3 * i + 1] = v; big[3 * i + 2] = v;
    }
    VibFeatures f;
    ASSERT(features_from_counts(big, FEATURES_MAX_SAMPLES, 1.0, &f) == ALGAOPS_OK, "max n");
    ASSERT_NEAR(f.rms_g[0], 32767.5, 1e-6, "no int64 overflow at extremes");
    ASSERT_NEAR(f.p2p_g[0], 65535.0, 1e-9, "p2p at extremes");
    ASSERT(features_from_counts(big, FEATURES_MAX_SAMPLES + 1, 1.0, &f) == ALGAOPS_ERR_RANGE,
           "n above limit rejected");
    ASSERT(features_from_counts(big, 1, 1.0, &f) == ALGAOPS_ERR_RANGE, "n < 2 rejected");
    ASSERT(features_from_counts(big, 10, 0.0, &f) == ALGAOPS_ERR_RANGE, "scale must be > 0");
    ASSERT(features_from_counts(NULL, 10, 1.0, &f) == ALGAOPS_ERR_NULL_PTR, "null input");
    return 0;
}

static int test_spectral_support_rules(void)
{
    FeatureBand b1 = { 45.0, 52.0 };
    char why[160];
    ASSERT(features_spectral_supported(800.0, 1024, &b1, 1, 4, why, sizeof(why)),
           "800 Hz / 1024 => df 0.78 Hz, 7 Hz band ~9 bins");
    ASSERT(!features_spectral_supported(800.0, 1000, &b1, 1, 4, why, sizeof(why)),
           "non power-of-two window rejected");
    ASSERT(strstr(why, "power of two") != NULL, "reason given");
    FeatureBand hi = { 300.0, 380.0 };
    ASSERT(!features_spectral_supported(800.0, 1024, &hi, 1, 4, why, sizeof(why)),
           "band above 0.4 fs rejected");
    FeatureBand narrow = { 48.0, 49.0 };
    ASSERT(!features_spectral_supported(800.0, 256, &narrow, 1, 4, why, sizeof(why)),
           "insufficient resolution rejected (df 3.1 Hz)");
    return 0;
}

static int test_fft_matches_dft(void)
{
#define N 64
    static float data[2 * N];
    double re_in[N];
    for (int i = 0; i < N; i++) {
        re_in[i] = sin(2 * PI * 5 * i / N) + 0.5 * cos(2 * PI * 11 * i / N) + 0.1 * i / N;
        data[2 * i] = (float)re_in[i];
        data[2 * i + 1] = 0.0f;
    }
    ASSERT(features_fft(data, N) == ALGAOPS_OK, "fft");
    for (int k = 0; k < N; k++) {
        double sr = 0, si = 0;
        for (int n = 0; n < N; n++) {
            sr += re_in[n] * cos(-2 * PI * k * n / N);
            si += re_in[n] * sin(-2 * PI * k * n / N);
        }
        ASSERT_NEAR(data[2 * k], sr, 1e-3, "fft real part == DFT");
        ASSERT_NEAR(data[2 * k + 1], si, 1e-3, "fft imag part == DFT");
    }
    ASSERT(features_fft(data, 48) == ALGAOPS_ERR_RANGE, "non power of two rejected");
    return 0;
}
#undef N

static int test_band_rms_of_sine(void)
{
#define N 1024
    const double fs = 800.0, f0 = 64.0 * fs / N;   /* bin 64 => 50 Hz */
    const double amp_counts = 200.0, scale = 0.0039;
    for (int i = 0; i < N; i++) {
        double v = amp_counts * sin(2 * PI * f0 * i / fs);
        g_xyz[3 * i] = (int16_t)lround(v + 256.0);
        g_xyz[3 * i + 1] = 0;
        g_xyz[3 * i + 2] = 0;
    }
    FeatureBand bands[2] = { { 45.0, 55.0 }, { 95.0, 105.0 } };
    static float work[2 * N];
    double out[2];
    ASSERT(features_band_rms(g_xyz, N, 0, scale, fs, bands, 2, work, out) == ALGAOPS_OK, "band");
    double want = amp_counts * scale / sqrt(2.0);
    ASSERT_NEAR(out[0], want, want * 0.01, "1x band RMS equals sine RMS within 1%");
    ASSERT(out[1] < want * 0.02, "2x band nearly empty for a pure sine");
    VibFeatures f;
    features_from_counts(g_xyz, N, scale, &f);
    ASSERT_NEAR(out[0], f.rms_g[0], f.rms_g[0] * 0.02, "band RMS ~ total AC RMS for pure tone");
    double all[2];
    ASSERT(features_band_rms(g_xyz, N, 3, scale, fs, bands, 2, work, all) == ALGAOPS_OK, "vector");
    ASSERT_NEAR(all[0], out[0], 1e-6, "other axes contribute nothing");
    return 0;
}
#undef N

int main(void)
{
    printf("=== Vibration Feature Tests ===\n");
    RUN_TEST(test_square_wave_rms_and_p2p);
    RUN_TEST(test_frozen_detection);
    RUN_TEST(test_extreme_values_exact);
    RUN_TEST(test_spectral_support_rules);
    RUN_TEST(test_fft_matches_dft);
    RUN_TEST(test_band_rms_of_sine);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
