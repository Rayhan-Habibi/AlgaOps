/*
 * health_mahalanobis.c — regularized Mahalanobis distance to the baseline.
 *
 * Computed in standardized space: z_i = (x_i - mu_i) / s_i with
 * s_i = max(std_i, floor_i).  The standardized covariance C_ij = cov_ij /
 * (s_i s_j) is shrunk toward the identity, R = (1 - alpha) C + alpha I, which
 * keeps R positive definite for highly correlated or nearly constant
 * features.  D^2 = z^T R^-1 z via Cholesky; the default threshold is the
 * chi-square 0.999 quantile for the number of features used.
 */

#include "health_internal.h"
#include "linalg_small.h"

#include <math.h>

#define F HM_F

double health_chi2_999(int dims)
{
    static const double Q[5] = { 0.0, 10.828, 13.816, 16.266, 18.467 };
    return (dims >= 1 && dims <= 4) ? Q[dims] : 0.0;
}

void health_mahalanobis(const HealthMethodsConfig *cfg, const AlgaBaseline *bl,
                        const double x[ALGA_MAX_FEATURES], uint32_t present,
                        AlgaMethodResult *out)
{
    if (!out) return;
    health_method_reset(out, ALGA_METHOD_UNAVAILABLE);
    if (!cfg || !bl || !x || !bl->valid) return;

    uint32_t use = bl->feature_mask & cfg->feature_mask & present;
    int idx[F];
    int d = 0;
    for (int i = 0; i < F; i++) {
        if (use & ALGA_FMASK(i)) idx[d++] = i;
    }
    if (d == 0) return;
    /* Enough samples to estimate a d×d covariance? */
    if ((uint64_t)bl->n < (uint64_t)cfg->maha_min_samples_per_dim * (uint64_t)d) return;

    double s[F], z[F];
    for (int a = 0; a < d; a++) {
        int i = idx[a];
        s[a] = bl->std[i] < cfg->std_floor[i] ? cfg->std_floor[i] : bl->std[i];
        if (!(s[a] > 0.0)) return;
        z[a] = (x[i] - bl->mean[i]) / s[a];
        if (!isfinite(z[a])) return;
        out->feature_z[i] = fabs(z[a]);
    }

    double alpha = cfg->maha_shrinkage;
    if (!(alpha >= 0.0) || alpha >= 1.0) alpha = 0.10;
    double r[LINALG_MAX_N * LINALG_MAX_N];
    for (int a = 0; a < d; a++) {
        for (int b = 0; b < d; b++) {
            double c = bl->cov[idx[a] * F + idx[b]] / (s[a] * s[b]);
            r[a * d + b] = (1.0 - alpha) * c + (a == b ? alpha : 0.0);
        }
    }
    if (linalg_cholesky(r, d) != 0) return;
    double d2 = linalg_mahalanobis_sq(r, d, z);
    if (!isfinite(d2)) return;

    double thr = cfg->maha_threshold > 0.0 ? cfg->maha_threshold : health_chi2_999(d);
    if (!(thr > 0.0)) return;

    int top = idx[0];
    double best = fabs(z[0]);
    for (int a = 1; a < d; a++) {
        if (fabs(z[a]) > best) { best = fabs(z[a]); top = idx[a]; }
    }
    out->status      = ALGA_METHOD_OK;
    out->dims        = d;
    out->score       = d2;
    out->threshold   = thr;
    out->anomalous   = d2 > thr;
    out->top_feature = top;
}

