/*
 * test_health_methods.c — fixed thresholds, z-score, regularized Mahalanobis.
 */

#include "test_harness.h"
#include "../include/health_methods.h"

/* Baseline with strongly correlated vibration and current (d = 2). */
static AlgaBaseline correlated_baseline(uint32_t n)
{
    AlgaBaselineBuilder b;
    alga_baseline_builder_init(&b, ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_CURRENT));
    uint32_t seed = 12345u;
    for (uint32_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        double u = (double)((seed >> 8) & 0xFFFF) / 65535.0 - 0.5;   /* [-0.5, 0.5] */
        seed = seed * 1103515245u + 12345u;
        double v = (double)((seed >> 8) & 0xFFFF) / 65535.0 - 0.5;
        double x[ALGA_MAX_FEATURES] = { 0.42 + 0.02 * u + 0.002 * v, 0.0, 0.0,
                                        1.80 + 0.10 * u };
        alga_baseline_builder_add(&b, x, ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_CURRENT));
    }
    AlgaBaseline bl;
    alga_baseline_builder_finish(&b, ALGA_MODE_STEADY, 2, &bl);
    return bl;
}

static int test_health_z_floor_and_non_finite(void)
{
    ASSERT_NEAR(health_z(1.0, 0.0, 0.0, 0.5), 2.0, 1e-12, "floor applied when std < floor");
    ASSERT_NEAR(health_z(1.0, 0.0, 2.0, 0.5), 0.5, 1e-12, "std used when above floor");
    ASSERT(health_z(INFINITY, 0.0, 1.0, 1e-6) == 0.0, "non-finite z maps to 0 (v1 rule)");
    return 0;
}

static int test_fixed_thresholds_per_mode(void)
{
    HealthMethodsConfig c = health_methods_default_config();
    c.fixed_vib_max_g[ALGA_MODE_STARTUP] = 1.5;
    double x[ALGA_MAX_FEATURES] = { 1.2, 0.0, 40.0, 0.0 };
    uint32_t present = ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP);
    AlgaMethodResult r;
    health_fixed(&c, ALGA_MODE_STEADY, x, present, &r);
    ASSERT(r.status == ALGA_METHOD_OK && r.anomalous, "1.2 g > 1.0 g steady limit");
    ASSERT(r.top_feature == ALGA_F_VIB_RMS && r.score > 1.0, "vibration drives the score");
    health_fixed(&c, ALGA_MODE_STARTUP, x, present, &r);
    ASSERT(r.status == ALGA_METHOD_OK && !r.anomalous, "1.2 g within the startup limit");
    x[ALGA_F_TEMP] = 61.0;
    health_fixed(&c, ALGA_MODE_STARTUP, x, present, &r);
    ASSERT(r.anomalous && r.top_feature == ALGA_F_TEMP, "temperature limit");
    for (int m = 0; m < ALGA_MODE_COUNT; m++) {
        c.fixed_vib_max_g[m] = 0; c.fixed_temp_max_c[m] = 0;
    }
    health_fixed(&c, ALGA_MODE_STEADY, x, present, &r);
    ASSERT(r.status == ALGA_METHOD_UNAVAILABLE, "no limits => unavailable");
    return 0;
}

static int test_zscore_requires_baseline(void)
{
    HealthMethodsConfig c = health_methods_default_config();
    double x[ALGA_MAX_FEATURES] = { 0.42, 0, 28, 0 };
    AlgaMethodResult r;
    health_zscore(&c, NULL, x, 0x5u, &r);
    ASSERT(r.status == ALGA_METHOD_UNAVAILABLE, "no baseline => unavailable");
    AlgaBaseline bl;
    memset(&bl, 0, sizeof(bl));
    bl.valid = 1; bl.n = 30; bl.mode = ALGA_MODE_STEADY;
    bl.feature_mask = ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP);
    bl.mean[ALGA_F_VIB_RMS] = 0.4198; bl.std[ALGA_F_VIB_RMS] = 0.0024;
    bl.mean[ALGA_F_TEMP] = 28.15; bl.std[ALGA_F_TEMP] = 0.10;
    x[ALGA_F_VIB_RMS] = 1.85;
    health_zscore(&c, &bl, x, ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP), &r);
    ASSERT(r.status == ALGA_METHOD_OK && r.anomalous && r.dims == 2, "large vib deviation");
    ASSERT_NEAR(r.feature_z[ALGA_F_VIB_RMS], (1.85 - 0.4198) / 0.0024, 1e-9, "z value");
    x[ALGA_F_VIB_RMS] = 0.42;
    health_zscore(&c, &bl, x, ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP), &r);
    ASSERT(!r.anomalous, "near baseline is not anomalous");
    return 0;
}

static int test_mahalanobis_uses_correlation(void)
{
    HealthMethodsConfig c = health_methods_default_config();
    c.feature_mask = ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_CURRENT);
    AlgaBaseline bl = correlated_baseline(200);
    uint32_t present = c.feature_mask;
    double sv = bl.std[ALGA_F_VIB_RMS], sc = bl.std[ALGA_F_CURRENT];
    /* Point moving along the correlation (both up by 2 sigma) vs against it. */
    double along[ALGA_MAX_FEATURES]   = { bl.mean[0] + 2 * sv, 0, 0, bl.mean[3] + 2 * sc };
    double against[ALGA_MAX_FEATURES] = { bl.mean[0] + 2 * sv, 0, 0, bl.mean[3] - 2 * sc };
    AlgaMethodResult ra, rb, rz;
    health_mahalanobis(&c, &bl, along, present, &ra);
    health_mahalanobis(&c, &bl, against, present, &rb);
    ASSERT(ra.status == ALGA_METHOD_OK && rb.status == ALGA_METHOD_OK, "evaluated");
    ASSERT(ra.dims == 2 && ra.threshold > 13.8 && ra.threshold < 13.9, "chi2(0.999, 2)");
    ASSERT(rb.score > 5.0 * ra.score, "breaking the correlation is far more anomalous");
    ASSERT(!ra.anomalous && rb.anomalous, "only the decorrelated point is flagged");
    /* z-score cannot tell these apart: both are 2 sigma per feature. */
    health_zscore(&c, &bl, against, present, &rz);
    ASSERT(!rz.anomalous, "z-score misses the decorrelation (shows the method difference)");
    return 0;
}

static int test_mahalanobis_regularization_and_sample_rule(void)
{
    HealthMethodsConfig c = health_methods_default_config();
    c.feature_mask = ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_CURRENT);
    /* Perfectly collinear features: singular covariance. */
    AlgaBaselineBuilder b;
    alga_baseline_builder_init(&b, c.feature_mask);
    for (int i = 0; i < 40; i++) {
        double x[ALGA_MAX_FEATURES] = { 0.40 + 0.001 * i, 0, 0, 1.0 + 0.01 * i };
        alga_baseline_builder_add(&b, x, c.feature_mask);
    }
    AlgaBaseline bl;
    alga_baseline_builder_finish(&b, ALGA_MODE_STEADY, 2, &bl);
    double x[ALGA_MAX_FEATURES] = { 0.42, 0, 0, 1.2 };
    AlgaMethodResult r;
    health_mahalanobis(&c, &bl, x, c.feature_mask, &r);
    ASSERT(r.status == ALGA_METHOD_OK && isfinite(r.score), "shrinkage keeps R positive definite");
    c.maha_min_samples_per_dim = 30;   /* needs 60 samples for d = 2 */
    health_mahalanobis(&c, &bl, x, c.feature_mask, &r);
    ASSERT(r.status == ALGA_METHOD_UNAVAILABLE, "insufficient samples for the dimension");
    ASSERT(health_chi2_999(1) > 10.8 && health_chi2_999(4) > 18.4 && health_chi2_999(9) == 0.0,
           "chi-square table");
    ASSERT(health_method_parse("mahalanobis") == ALGA_HM_MAHALANOBIS &&
           health_method_parse("bogus") == -1, "method names");
    return 0;
}

int main(void)
{
    printf("=== Health Method Tests ===\n");
    RUN_TEST(test_health_z_floor_and_non_finite);
    RUN_TEST(test_fixed_thresholds_per_mode);
    RUN_TEST(test_zscore_requires_baseline);
    RUN_TEST(test_mahalanobis_uses_correlation);
    RUN_TEST(test_mahalanobis_regularization_and_sample_rule);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
