/* sample_window.c — see sample_window.h */

#include "sample_window.h"

#include <math.h>
#include <string.h>

bool sample_window_init(sample_window_t *w, int16_t *xyz, uint32_t samples_expected)
{
    if (!w || !xyz || samples_expected < 2 || samples_expected > FEATURES_MAX_SAMPLES) {
        return false;
    }
    memset(w, 0, sizeof(*w));
    w->xyz = xyz;
    w->samples_expected = samples_expected;
    return true;
}

void sample_window_reset(sample_window_t *w)
{
    if (!w) return;
    w->samples_valid = 0;
    w->samples_lost  = 0;
    w->overrun       = false;
}

static uint32_t room(const sample_window_t *w)
{
    uint32_t used = w->samples_valid + w->samples_lost;
    return used >= w->samples_expected ? 0 : w->samples_expected - used;
}

uint32_t sample_window_add(sample_window_t *w, const int16_t *xyz, uint32_t n)
{
    if (!w || !xyz) return 0;
    uint32_t take = n < room(w) ? n : room(w);
    memcpy(w->xyz + 3u * w->samples_valid, xyz, (size_t)take * 3u * sizeof(int16_t));
    w->samples_valid += take;
    return take;
}

void sample_window_mark_overrun(sample_window_t *w, uint32_t lost)
{
    if (!w) return;
    if (lost == 0) lost = 1;
    uint32_t r = room(w);
    w->samples_lost += lost < r ? lost : r;
    w->overrun = true;
}

bool sample_window_full(const sample_window_t *w)
{
    return w && room(w) == 0;
}

AlgaopsStatus sample_window_features(const sample_window_t *w, double g_per_lsb,
                                     VibFeatures *out)
{
    if (!w || !out) return ALGAOPS_ERR_NULL_PTR;
    return features_from_counts(w->xyz, w->samples_valid, g_per_lsb, out);
}

/* ---- AC RMS ---------------------------------------------------------------- */

void ac_rms_reset(ac_rms_acc_t *a)
{
    if (a) memset(a, 0, sizeof(*a));
}

void ac_rms_add(ac_rms_acc_t *a, double v, bool clipped)
{
    if (!a || !isfinite(v)) return;
    if (a->n == 0) a->shift = v;
    double d = v - a->shift;
    a->sum += d;
    a->sum_sq += d * d;
    a->n++;
    if (clipped) a->clipped++;
}

void ac_rms_error(ac_rms_acc_t *a)
{
    if (a) a->errors++;
}

bool ac_rms_result(const ac_rms_acc_t *a, double *rms)
{
    if (!a || !rms || a->n < 2) return false;
    double n = (double)a->n;
    double var = (a->sum_sq - a->sum * a->sum / n) / n;
    *rms = var > 0.0 ? sqrt(var) : 0.0;
    return true;
}
