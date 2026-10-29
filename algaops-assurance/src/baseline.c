/*
 * baseline.c — multivariate Welford baseline builder.
 */

#include "../include/baseline.h"

#include <math.h>
#include <string.h>

#define F ALGA_MAX_FEATURES

uint32_t alga_window_features(const AlgaWindow *w, double x[ALGA_MAX_FEATURES])
{
    for (int i = 0; i < F; i++) x[i] = 0.0;
    if (!w) return 0;
    uint32_t mask = 0;
    if (isfinite(w->vibration_rms_g)) {
        x[ALGA_F_VIB_RMS] = w->vibration_rms_g;
        mask |= ALGA_FMASK(ALGA_F_VIB_RMS);
    }
    if (w->presence & ALGA_HAS_P2P) {
        double p = w->vib_p2p_axis_g[0];
        if (w->vib_p2p_axis_g[1] > p) p = w->vib_p2p_axis_g[1];
        if (w->vib_p2p_axis_g[2] > p) p = w->vib_p2p_axis_g[2];
        if (isfinite(p)) {
            x[ALGA_F_VIB_P2P] = p;
            mask |= ALGA_FMASK(ALGA_F_VIB_P2P);
        }
    }
    if (isfinite(w->temperature_c)) {
        x[ALGA_F_TEMP] = w->temperature_c;
        mask |= ALGA_FMASK(ALGA_F_TEMP);
    }
    if ((w->presence & ALGA_HAS_CURRENT) && isfinite(w->current_a)) {
        x[ALGA_F_CURRENT] = w->current_a;
        mask |= ALGA_FMASK(ALGA_F_CURRENT);
    }
    return mask;
}

void alga_baseline_builder_init(AlgaBaselineBuilder *b, uint32_t requested_mask)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->requested_mask = requested_mask & ((1u << F) - 1u);
    b->seen_mask = b->requested_mask;
}

AlgaopsStatus alga_baseline_builder_add(AlgaBaselineBuilder *b,
                                        const double x[ALGA_MAX_FEATURES],
                                        uint32_t mask)
{
    if (!b || !x) return ALGAOPS_ERR_NULL_PTR;
    uint32_t use = mask & b->requested_mask;
    for (int i = 0; i < F; i++) {
        if ((use & ALGA_FMASK(i)) && !isfinite(x[i])) return ALGAOPS_ERR_RANGE;
    }
    b->seen_mask &= use;
    b->n++;
    double delta[F];
    for (int i = 0; i < F; i++) {
        delta[i] = (use & ALGA_FMASK(i)) ? x[i] - b->mean[i] : 0.0;
        if (use & ALGA_FMASK(i)) b->mean[i] += delta[i] / (double)b->n;
    }
    for (int i = 0; i < F; i++) {
        if (!(use & ALGA_FMASK(i))) continue;
        for (int j = 0; j < F; j++) {
            if (!(use & ALGA_FMASK(j))) continue;
            b->comoment[i * F + j] += delta[i] * (x[j] - b->mean[j]);
        }
    }
    return ALGAOPS_OK;
}

AlgaopsStatus alga_baseline_builder_add_window(AlgaBaselineBuilder *b,
                                               const AlgaWindow *w)
{
    if (!b || !w) return ALGAOPS_ERR_NULL_PTR;
    double x[F];
    uint32_t mask = alga_window_features(w, x);
    return alga_baseline_builder_add(b, x, mask);
}

AlgaopsStatus alga_baseline_builder_finish(const AlgaBaselineBuilder *b,
                                           int mode, uint32_t min_samples,
                                           AlgaBaseline *out)
{
    if (!b || !out) return ALGAOPS_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    out->mode = mode;
    out->n = b->n;
    uint32_t mask = b->seen_mask & b->requested_mask;
    if (b->n == 0 || b->n < min_samples || mask == 0) {
        out->valid = 0;
        return ALGAOPS_ERR_BASELINE;
    }
    out->feature_mask = mask;
    for (int i = 0; i < F; i++) {
        if (!(mask & ALGA_FMASK(i))) continue;
        out->mean[i] = b->mean[i];
        double var = b->n < 2 ? 0.0 : b->comoment[i * F + i] / (double)b->n;
        out->std[i] = var > 0.0 ? sqrt(var) : 0.0;
        for (int j = 0; j < F; j++) {
            if (!(mask & ALGA_FMASK(j))) continue;
            out->cov[i * F + j] = b->n < 2 ? 0.0 : b->comoment[i * F + j] / (double)b->n;
        }
    }
    out->valid = 1;
    return ALGAOPS_OK;
}

AlgaopsStatus alga_baseline_validate(const AlgaBaseline *b)
{
    if (!b) return ALGAOPS_ERR_NULL_PTR;
    if (!b->valid || b->n == 0) return ALGAOPS_ERR_BASELINE;
    if (b->mode < ALGA_MODE_OFF || b->mode > ALGA_MODE_STEADY) return ALGAOPS_ERR_BASELINE;
    uint32_t mask = b->feature_mask;
    if (mask == 0 || (mask & ~((1u << F) - 1u))) return ALGAOPS_ERR_BASELINE;
    for (int i = 0; i < F; i++) {
        if (!(mask & ALGA_FMASK(i))) continue;
        if (!isfinite(b->mean[i]) || !isfinite(b->std[i]) || b->std[i] < 0.0) {
            return ALGAOPS_ERR_BASELINE;
        }
        for (int j = 0; j < F; j++) {
            if (!(mask & ALGA_FMASK(j))) continue;
            double cij = b->cov[i * F + j], cji = b->cov[j * F + i];
            if (!isfinite(cij)) return ALGAOPS_ERR_BASELINE;
            if (fabs(cij - cji) > 1e-9 * (1.0 + fabs(cij))) return ALGAOPS_ERR_BASELINE;
        }
    }
    return ALGAOPS_OK;
}

int alga_feature_count(uint32_t mask)
{
    int c = 0;
    for (int i = 0; i < F; i++) {
        if (mask & ALGA_FMASK(i)) c++;
    }
    return c;
}

const char *alga_feature_name(int index)
{
    switch (index) {
        case ALGA_F_VIB_RMS: return "vib_rms_g";
        case ALGA_F_VIB_P2P: return "vib_p2p_g";
        case ALGA_F_TEMP:    return "temp_c";
        case ALGA_F_CURRENT: return "current_a";
        default:             return "unknown";
    }
}
