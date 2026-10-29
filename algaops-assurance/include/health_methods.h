/**
 * @file health_methods.h
 * @brief Equipment-health comparators: fixed thresholds, baseline z-score and
 *        regularized Mahalanobis distance.
 *
 * The engine runs every applicable method on each trusted window so that the
 * methods can be compared (shadow evaluations), but only the primary method
 * configured for the operating mode decides the outcome.  Methods never run
 * on untrusted windows.
 *
 * DISCLAIMER: all thresholds are preliminary engineering parameters, not
 * validated industrial diagnostic limits.
 */

#ifndef HEALTH_METHODS_H
#define HEALTH_METHODS_H

#include "algaops_types.h"
#include "baseline.h"

typedef struct {
    int32_t status;      /**< ALGA_METHOD_SKIPPED / OK / UNAVAILABLE          */
    int32_t anomalous;   /**< 1 when this method flags the window             */
    int32_t top_feature; /**< feature index driving the score, -1 if none     */
    int32_t dims;        /**< number of features evaluated                    */
    double  score;       /**< fixed: max x/limit; zscore: max |z|; maha: D²   */
    double  threshold;   /**< score threshold applied                         */
    double  feature_z[ALGA_MAX_FEATURES]; /**< per-feature |z| (z/maha) or ratio (fixed) */
} AlgaMethodResult;

typedef struct {
    /** Absolute limits per mode (index = AlgaMode). <= 0 disables a limit. */
    double   fixed_vib_max_g[ALGA_MODE_COUNT];
    double   fixed_temp_max_c[ALGA_MODE_COUNT];
    double   fixed_current_max_a[ALGA_MODE_COUNT];
    /** z-score threshold (strict >). */
    double   z_threshold;
    /** Standard-deviation floors per feature (division safety, noise floor). */
    double   std_floor[ALGA_MAX_FEATURES];
    /** D² threshold; <= 0 selects the chi-square 0.999 quantile for d dims. */
    double   maha_threshold;
    /** Shrinkage alpha toward identity in standardized space, [0, 1). */
    double   maha_shrinkage;
    /** Mahalanobis requires n >= maha_min_samples_per_dim * d. */
    uint32_t maha_min_samples_per_dim;
    /** Features used by z-score / Mahalanobis (subset of baseline mask). */
    uint32_t feature_mask;
    /**
     * Apply the fixed absolute limits in every mode in addition to the
     * primary method: a trusted window above a fixed limit is CHECK_EQUIPMENT
     * even when the primary method finds no deviation or has no baseline.
     * Default false (v1 behaviour).
     */
    bool     fixed_always_on;
} HealthMethodsConfig;

static inline HealthMethodsConfig health_methods_default_config(void)
{
    HealthMethodsConfig c;
    for (int m = 0; m < ALGA_MODE_COUNT; m++) {
        c.fixed_vib_max_g[m]     = 1.0;   /* [CONFIGURABLE] preliminary */
        c.fixed_temp_max_c[m]    = 60.0;  /* [CONFIGURABLE] preliminary */
        c.fixed_current_max_a[m] = 0.0;   /* disabled */
    }
    c.z_threshold = 3.0;
    for (int i = 0; i < ALGA_MAX_FEATURES; i++) c.std_floor[i] = 1e-6;
    c.maha_threshold = 0.0;
    c.maha_shrinkage = 0.10;
    c.maha_min_samples_per_dim = 10;
    c.feature_mask = ALGA_DEFAULT_FEATURE_MASK;
    c.fixed_always_on = false;
    return c;
}

/** |x - mean| / max(std, floor); non-finite results map to 0 (v1 rule). */
double health_z(double x, double mean, double std, double floor);

/** Chi-square 0.999 quantile for 1..4 degrees of freedom (0 otherwise). */
double health_chi2_999(int dims);

void health_fixed(const HealthMethodsConfig *cfg, int mode,
                  const double x[ALGA_MAX_FEATURES], uint32_t present,
                  AlgaMethodResult *out);

void health_zscore(const HealthMethodsConfig *cfg, const AlgaBaseline *bl,
                   const double x[ALGA_MAX_FEATURES], uint32_t present,
                   AlgaMethodResult *out);

void health_mahalanobis(const HealthMethodsConfig *cfg, const AlgaBaseline *bl,
                        const double x[ALGA_MAX_FEATURES], uint32_t present,
                        AlgaMethodResult *out);

/** Constant label for an AlgaHealthMethod value. */
const char *health_method_str(int method);

/** Parse "fixed" / "zscore" / "mahalanobis"; returns -1 when unknown. */
int health_method_parse(const char *s);

#endif /* HEALTH_METHODS_H */
