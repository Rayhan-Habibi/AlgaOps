/*
 * test_baseline.c — multivariate Welford baseline builder.
 */

#include "test_harness.h"
#include "../include/baseline.h"

static const double VIB[10]  = { 0.420, 0.415, 0.425, 0.418, 0.422, 0.419, 0.421, 0.417, 0.423, 0.420 };
static const double TEMP[10] = { 28.1, 28.2, 28.0, 28.3, 28.1, 28.2, 28.1, 28.3, 28.0, 28.2 };

static int test_mean_std_cov_match_two_pass(void)
{
    AlgaBaselineBuilder b;
    alga_baseline_builder_init(&b, ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP));
    for (int i = 0; i < 10; i++) {
        double x[ALGA_MAX_FEATURES] = { VIB[i], 0.0, TEMP[i], 0.0 };
        ASSERT(alga_baseline_builder_add(&b, x, ALGA_FMASK(ALGA_F_VIB_RMS) |
                                         ALGA_FMASK(ALGA_F_TEMP)) == ALGAOPS_OK, "add");
    }
    AlgaBaseline bl;
    ASSERT(alga_baseline_builder_finish(&b, ALGA_MODE_STEADY, 10, &bl) == ALGAOPS_OK, "finish");
    double mv = 0, mt = 0;
    for (int i = 0; i < 10; i++) { mv += VIB[i]; mt += TEMP[i]; }
    mv /= 10; mt /= 10;
    double vv = 0, vt = 0, cvt = 0;
    for (int i = 0; i < 10; i++) {
        vv += (VIB[i] - mv) * (VIB[i] - mv);
        vt += (TEMP[i] - mt) * (TEMP[i] - mt);
        cvt += (VIB[i] - mv) * (TEMP[i] - mt);
    }
    ASSERT_NEAR(bl.mean[ALGA_F_VIB_RMS], mv, 1e-12, "vib mean");
    ASSERT_NEAR(bl.mean[ALGA_F_TEMP], mt, 1e-12, "temp mean");
    ASSERT_NEAR(bl.std[ALGA_F_VIB_RMS], sqrt(vv / 10), 1e-12, "population std (n)");
    ASSERT_NEAR(bl.std[ALGA_F_TEMP], sqrt(vt / 10), 1e-12, "temp std");
    ASSERT_NEAR(bl.cov[ALGA_F_VIB_RMS * ALGA_MAX_FEATURES + ALGA_F_TEMP], cvt / 10, 1e-12, "cov");
    ASSERT_NEAR(bl.cov[ALGA_F_TEMP * ALGA_MAX_FEATURES + ALGA_F_VIB_RMS], cvt / 10, 1e-12, "sym");
    ASSERT(bl.valid && bl.n == 10 && bl.mode == ALGA_MODE_STEADY, "metadata");
    ASSERT(alga_baseline_validate(&bl) == ALGAOPS_OK, "validates");
    return 0;
}

static int test_min_samples_and_feature_intersection(void)
{
    AlgaBaselineBuilder b;
    alga_baseline_builder_init(&b, ALGA_DEFAULT_FEATURE_MASK);
    AlgaBaseline bl;
    double x[ALGA_MAX_FEATURES] = { 0.42, 0.0, 28.0, 1.8 };
    for (int i = 0; i < 4; i++) {
        uint32_t mask = ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP);
        if (i != 2) mask |= ALGA_FMASK(ALGA_F_CURRENT);   /* current missing once */
        x[ALGA_F_VIB_RMS] = 0.42 + 0.01 * i;
        alga_baseline_builder_add(&b, x, mask);
    }
    ASSERT(alga_baseline_builder_finish(&b, ALGA_MODE_STEADY, 10, &bl) == ALGAOPS_ERR_BASELINE,
           "fewer than min_samples rejected");
    ASSERT(!bl.valid, "invalid when rejected");
    ASSERT(alga_baseline_builder_finish(&b, ALGA_MODE_STEADY, 3, &bl) == ALGAOPS_OK, "accepted");
    ASSERT(!(bl.feature_mask & ALGA_FMASK(ALGA_F_CURRENT)),
           "a feature missing in any sample is excluded");
    ASSERT(bl.feature_mask & ALGA_FMASK(ALGA_F_VIB_RMS), "vib kept");
    return 0;
}

static int test_non_finite_sample_rejected(void)
{
    AlgaBaselineBuilder b;
    alga_baseline_builder_init(&b, ALGA_DEFAULT_FEATURE_MASK);
    double x[ALGA_MAX_FEATURES] = { NAN, 0.0, 28.0, 0.0 };
    ASSERT(alga_baseline_builder_add(&b, x, ALGA_FMASK(ALGA_F_VIB_RMS)) == ALGAOPS_ERR_RANGE,
           "NaN sample rejected");
    ASSERT(b.n == 0, "rejected sample not counted");
    return 0;
}

static int test_validate_rejects_corruption(void)
{
    AlgaBaseline bl;
    memset(&bl, 0, sizeof(bl));
    bl.valid = 1; bl.n = 20; bl.mode = ALGA_MODE_STEADY;
    bl.feature_mask = ALGA_FMASK(ALGA_F_VIB_RMS);
    bl.mean[0] = 0.42; bl.std[0] = 0.002; bl.cov[0] = 4e-6;
    ASSERT(alga_baseline_validate(&bl) == ALGAOPS_OK, "sane baseline");
    bl.std[0] = -1.0;
    ASSERT(alga_baseline_validate(&bl) != ALGAOPS_OK, "negative std rejected");
    bl.std[0] = 0.002; bl.mean[0] = INFINITY;
    ASSERT(alga_baseline_validate(&bl) != ALGAOPS_OK, "non-finite mean rejected");
    bl.mean[0] = 0.42; bl.mode = 9;
    ASSERT(alga_baseline_validate(&bl) != ALGAOPS_OK, "invalid mode rejected");
    bl.mode = ALGA_MODE_STEADY; bl.feature_mask = 0x80;
    ASSERT(alga_baseline_validate(&bl) != ALGAOPS_OK, "unknown feature bits rejected");
    return 0;
}

static int test_window_feature_extraction(void)
{
    AlgaWindow w;
    memset(&w, 0, sizeof(w));
    w.vibration_rms_g = 0.5;
    w.temperature_c = 31.0;
    double x[ALGA_MAX_FEATURES];
    ASSERT(alga_window_features(&w, x) == (ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP)),
           "v1 window has vib + temp");
    w.presence = ALGA_HAS_P2P | ALGA_HAS_CURRENT;
    w.vib_p2p_axis_g[0] = 1.2; w.vib_p2p_axis_g[1] = 1.9; w.vib_p2p_axis_g[2] = 0.7;
    w.current_a = 2.2;
    uint32_t m = alga_window_features(&w, x);
    ASSERT(m == 0xFu, "all four features present");
    ASSERT_NEAR(x[ALGA_F_VIB_P2P], 1.9, 1e-12, "p2p feature = max axis p2p");
    ASSERT_NEAR(x[ALGA_F_CURRENT], 2.2, 1e-12, "current feature");
    ASSERT(strcmp(alga_feature_name(ALGA_F_TEMP), "temp_c") == 0, "feature names");
    return 0;
}

int main(void)
{
    printf("=== Baseline Tests ===\n");
    RUN_TEST(test_mean_std_cov_match_two_pass);
    RUN_TEST(test_min_samples_and_feature_intersection);
    RUN_TEST(test_non_finite_sample_rejected);
    RUN_TEST(test_validate_rejects_corruption);
    RUN_TEST(test_window_feature_extraction);
    TEST_SUMMARY();
    return TEST_EXIT_CODE();
}
