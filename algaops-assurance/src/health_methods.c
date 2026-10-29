/*
 * health_methods.c — z-score against the per-mode baseline and the method
 * names.  Fixed limits: health_fixed_threshold.c; regularized Mahalanobis:
 * health_mahalanobis.c.
 */

#include "health_internal.h"

#include <math.h>
#include <string.h>

#define F HM_F

double health_z(double x, double mean, double std, double floor)
{
    double s = (std < floor) ? floor : std;
    double z = fabs(x - mean) / s;
    if (!isfinite(z)) z = 0.0;
    return z;
}

void health_zscore(const HealthMethodsConfig *cfg, const AlgaBaseline *bl,
                   const double x[ALGA_MAX_FEATURES], uint32_t present,
                   AlgaMethodResult *out)
{
    if (!out) return;
    health_method_reset(out, ALGA_METHOD_UNAVAILABLE);
    if (!cfg || !bl || !x || !bl->valid) return;

    uint32_t use = bl->feature_mask & cfg->feature_mask & present;
    int dims = 0;
    double best = 0.0;
    for (int i = 0; i < F; i++) {
        if (!(use & ALGA_FMASK(i))) continue;
        double z = health_z(x[i], bl->mean[i], bl->std[i], cfg->std_floor[i]);
        out->feature_z[i] = z;
        if (dims == 0 || z > best) { best = z; out->top_feature = i; }
        dims++;
    }
    if (dims == 0) return;
    out->status    = ALGA_METHOD_OK;
    out->dims      = dims;
    out->score     = best;
    out->threshold = cfg->z_threshold;
    out->anomalous = best > cfg->z_threshold;
}

const char *health_method_str(int method)
{
    switch (method) {
        case ALGA_HM_FIXED:       return "fixed";
        case ALGA_HM_ZSCORE:      return "zscore";
        case ALGA_HM_MAHALANOBIS: return "mahalanobis";
        default:                  return "none";
    }
}

int health_method_parse(const char *s)
{
    if (!s) return -1;
    if (strcmp(s, "fixed") == 0)       return ALGA_HM_FIXED;
    if (strcmp(s, "zscore") == 0)      return ALGA_HM_ZSCORE;
    if (strcmp(s, "mahalanobis") == 0) return ALGA_HM_MAHALANOBIS;
    return -1;
}
