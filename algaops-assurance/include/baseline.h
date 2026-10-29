/**
 * @file baseline.h
 * @brief Per-mode baseline statistics (mean, std, covariance) of health features.
 *
 * Health feature vector (see ALGA_F_* in algaops_types.h):
 *   [0] vibration RMS magnitude (g)   always present
 *   [1] max per-axis peak-to-peak (g) when the window carries p2p
 *   [2] surface temperature (°C)      always present
 *   [3] RMS current (A)               when the window carries current
 *
 * Statistics use the numerically stable multivariate Welford update and the
 * population (n) denominator, identical to the v1 Equipment Health baseline.
 *
 * DISCLAIMER: baselines computed from synthetic or short recordings are not
 * validated industrial thresholds.  Re-capture on the real pump/blower.
 */

#ifndef BASELINE_H
#define BASELINE_H

#include "algaops_types.h"
#include "algaops_export.h"

typedef struct {
    uint32_t id;                                   /**< host-assigned id       */
    int32_t  mode;                                 /**< AlgaMode               */
    uint32_t feature_mask;                         /**< ALGA_FMASK bits        */
    uint32_t n;                                    /**< samples used           */
    int32_t  valid;                                /**< 1 when usable          */
    double   mean[ALGA_MAX_FEATURES];
    double   std[ALGA_MAX_FEATURES];
    double   cov[ALGA_MAX_FEATURES * ALGA_MAX_FEATURES]; /**< row-major        */
} AlgaBaseline;

typedef struct {
    uint32_t requested_mask;
    uint32_t seen_mask;       /**< features present in every added sample */
    uint32_t n;
    double   mean[ALGA_MAX_FEATURES];
    double   comoment[ALGA_MAX_FEATURES * ALGA_MAX_FEATURES];
} AlgaBaselineBuilder;

/** Default health feature selection: vibration RMS, temperature, current. */
#define ALGA_DEFAULT_FEATURE_MASK \
    (ALGA_FMASK(ALGA_F_VIB_RMS) | ALGA_FMASK(ALGA_F_TEMP) | ALGA_FMASK(ALGA_F_CURRENT))

/**
 * @brief Extract the health feature vector of a window.
 * @return mask of features present (and finite) in this window.
 */
ALGAOPS_API uint32_t alga_window_features(const AlgaWindow *w,
                                          double x[ALGA_MAX_FEATURES]);

ALGAOPS_API void alga_baseline_builder_init(AlgaBaselineBuilder *b,
                                            uint32_t requested_mask);

/** Add one feature vector whose present features are given by @p mask. */
ALGAOPS_API AlgaopsStatus alga_baseline_builder_add(AlgaBaselineBuilder *b,
                                                    const double x[ALGA_MAX_FEATURES],
                                                    uint32_t mask);

/** Convenience: extract features from a window and add them. */
ALGAOPS_API AlgaopsStatus alga_baseline_builder_add_window(AlgaBaselineBuilder *b,
                                                           const AlgaWindow *w);

/**
 * @brief Finish the baseline.
 * @return ALGAOPS_OK, or ALGAOPS_ERR_BASELINE when fewer than @p min_samples
 *         samples were added or no requested feature was present in all of them.
 */
ALGAOPS_API AlgaopsStatus alga_baseline_builder_finish(const AlgaBaselineBuilder *b,
                                                       int mode, uint32_t min_samples,
                                                       AlgaBaseline *out);

/** Structural validation of a baseline loaded from storage. */
ALGAOPS_API AlgaopsStatus alga_baseline_validate(const AlgaBaseline *b);

/** Number of features set in a mask. */
int alga_feature_count(uint32_t mask);

/** Constant label for a feature index ("vib_rms_g", ...). */
ALGAOPS_API const char *alga_feature_name(int index);

#endif /* BASELINE_H */
