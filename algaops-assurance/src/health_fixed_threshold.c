/*
 * health_fixed_threshold.c — absolute per-mode limits (fixed thresholds).
 *
 * The score of each limited feature is x / limit; the window is anomalous
 * when any ratio exceeds 1.  Limits <= 0 disable a feature.  No baseline is
 * needed, so this method also judges STARTUP/OFF and acts as the hard-limit
 * layer (health.fixed_always_on) on top of the baseline methods.
 */

#include "health_internal.h"

#include <math.h>

#define F HM_F

void health_fixed(const HealthMethodsConfig *cfg, int mode,
                  const double x[ALGA_MAX_FEATURES], uint32_t present,
                  AlgaMethodResult *out)
{
    if (!out) return;
    health_method_reset(out, ALGA_METHOD_UNAVAILABLE);
    if (!cfg || !x || mode < 0 || mode >= ALGA_MODE_COUNT) return;

    double limits[F] = { 0.0, 0.0, 0.0, 0.0 };
    limits[ALGA_F_VIB_RMS] = cfg->fixed_vib_max_g[mode];
    limits[ALGA_F_TEMP]    = cfg->fixed_temp_max_c[mode];
    limits[ALGA_F_CURRENT] = cfg->fixed_current_max_a[mode];

    int dims = 0;
    double best = 0.0;
    for (int i = 0; i < F; i++) {
        if (limits[i] <= 0.0 || !(present & ALGA_FMASK(i))) continue;
        double ratio = x[i] / limits[i];
        if (!isfinite(ratio)) ratio = 0.0;
        out->feature_z[i] = ratio;
        if (dims == 0 || ratio > best) { best = ratio; out->top_feature = i; }
        dims++;
    }
    if (dims == 0) return;
    out->status    = ALGA_METHOD_OK;
    out->dims      = dims;
    out->score     = best;
    out->threshold = 1.0;
    out->anomalous = best > 1.0;
}

