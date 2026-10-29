/*
 * features.c — vibration features (time domain + optional spectral bands).
 *
 * Portable C11, no allocation; compiled into the gateway core and into the
 * ESP32 firmware (components/algaops_features), so both sides share exactly
 * one definition of RMS and peak-to-peak.
 */

#include "../include/features.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef ALGAOPS_PI
#define ALGAOPS_PI 3.14159265358979323846
#endif

static bool is_pow2(uint32_t n) { return n && !(n & (n - 1u)); }

AlgaopsStatus features_from_counts(const int16_t *xyz, uint32_t n,
                                   double g_per_lsb, VibFeatures *out)
{
    if (!xyz || !out) return ALGAOPS_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    if (n < 2u || n > FEATURES_MAX_SAMPLES || !(g_per_lsb > 0.0)) return ALGAOPS_ERR_RANGE;

    int64_t sum[3] = { 0, 0, 0 };
    int64_t sum_sq[3] = { 0, 0, 0 };
    int32_t mn[3], mx[3];
    for (int a = 0; a < 3; a++) { mn[a] = xyz[a]; mx[a] = xyz[a]; }

    for (uint32_t i = 0; i < n; i++) {
        for (int a = 0; a < 3; a++) {
            int32_t v = xyz[(size_t)i * 3u + (size_t)a];
            sum[a]    += v;
            sum_sq[a] += (int64_t)v * (int64_t)v;
            if (v < mn[a]) mn[a] = v;
            if (v > mx[a]) mx[a] = v;
        }
    }

    double mag_sq = 0.0;
    bool frozen = true;
    out->n = n;
    for (int a = 0; a < 3; a++) {
        /* exact: N·Σx² − (Σx)²  (both terms < 2^57 for N <= 8192) */
        int64_t num = (int64_t)n * sum_sq[a] - sum[a] * sum[a];
        if (num < 0) num = 0;
        double var_counts = (double)num / ((double)n * (double)n);
        double rms = sqrt(var_counts) * g_per_lsb;
        out->mean_g[a] = ((double)sum[a] / (double)n) * g_per_lsb;
        out->rms_g[a]  = rms;
        out->p2p_g[a]  = (double)(mx[a] - mn[a]) * g_per_lsb;
        if (mx[a] != mn[a]) frozen = false;
        mag_sq += rms * rms;
        if (out->p2p_g[a] > out->p2p_max_g) out->p2p_max_g = out->p2p_g[a];
    }
    out->rms_mag_g = sqrt(mag_sq);
    out->frozen = frozen;
    return ALGAOPS_OK;
}

bool features_spectral_supported(double fs_hz, uint32_t n,
                                 const FeatureBand *bands, int nbands,
                                 uint32_t min_bins_per_band,
                                 char *why, size_t why_len)
{
    if (why && why_len) why[0] = '\0';
#define UNSUPPORTED(...)                                       \
    do { if (why && why_len) snprintf(why, why_len, __VA_ARGS__); \
         return false; } while (0)

    if (!(fs_hz > 0.0) || !isfinite(fs_hz)) UNSUPPORTED("invalid sample rate");
    if (!is_pow2(n) || n < 64u || n > FEATURES_MAX_FFT)
        UNSUPPORTED("window length %u is not a power of two in [64, %u]",
                    (unsigned)n, (unsigned)FEATURES_MAX_FFT);
    if (!bands || nbands < 1 || nbands > ALGA_MAX_BANDS)
        UNSUPPORTED("band count must be 1..%d", ALGA_MAX_BANDS);
    double df = fs_hz / (double)n;
    for (int b = 0; b < nbands; b++) {
        double lo = bands[b].f_lo_hz, hi = bands[b].f_hi_hz;
        if (!(lo > 0.0) || !(hi > lo))
            UNSUPPORTED("band %d has invalid edges", b);
        if (hi > 0.4 * fs_hz)
            UNSUPPORTED("band %d upper edge %.1f Hz exceeds 0.4*fs = %.1f Hz",
                        b, hi, 0.4 * fs_hz);
        if ((hi - lo) / df < (double)min_bins_per_band)
            UNSUPPORTED("band %d spans %.2f bins (< %u) at df = %.3f Hz",
                        b, (hi - lo) / df, (unsigned)min_bins_per_band, df);
    }
#undef UNSUPPORTED
    return true;
}

AlgaopsStatus features_fft(float *data, uint32_t n)
{
    if (!data) return ALGAOPS_ERR_NULL_PTR;
    if (!is_pow2(n) || n > FEATURES_MAX_FFT) return ALGAOPS_ERR_RANGE;

    /* bit-reversal permutation */
    for (uint32_t i = 1, j = 0; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float tr = data[2 * i], ti = data[2 * i + 1];
            data[2 * i] = data[2 * j];
            data[2 * i + 1] = data[2 * j + 1];
            data[2 * j] = tr;
            data[2 * j + 1] = ti;
        }
    }
    for (uint32_t len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * ALGAOPS_PI / (double)len;
        for (uint32_t k = 0; k < len / 2u; k++) {
            float wr = (float)cos(ang * (double)k);
            float wi = (float)sin(ang * (double)k);
            for (uint32_t i = k; i < n; i += len) {
                uint32_t j = i + len / 2u;
                float xr = data[2 * j] * wr - data[2 * j + 1] * wi;
                float xi = data[2 * j] * wi + data[2 * j + 1] * wr;
                data[2 * j]     = data[2 * i] - xr;
                data[2 * j + 1] = data[2 * i + 1] - xi;
                data[2 * i]     += xr;
                data[2 * i + 1] += xi;
            }
        }
    }
    return ALGAOPS_OK;
}

static AlgaopsStatus band_power_axis(const int16_t *xyz, uint32_t n, int axis,
                                     double g_per_lsb, double fs_hz,
                                     const FeatureBand *bands, int nbands,
                                     float *work, double *power)
{
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += xyz[(size_t)i * 3u + (size_t)axis];
    double mean = (double)sum / (double)n;
    double w_sq = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2.0 * ALGAOPS_PI * (double)i / (double)n);
        double v = ((double)xyz[(size_t)i * 3u + (size_t)axis] - mean) * g_per_lsb;
        work[2 * i]     = (float)(v * w);
        work[2 * i + 1] = 0.0f;
        w_sq += w * w;
    }
    AlgaopsStatus s = features_fft(work, n);
    if (s != ALGAOPS_OK) return s;
    double df = fs_hz / (double)n;
    for (int b = 0; b < nbands; b++) {
        int64_t k_lo = (int64_t)ceil(bands[b].f_lo_hz / df);
        int64_t k_hi = (int64_t)floor(bands[b].f_hi_hz / df);
        if (k_lo < 1) k_lo = 1;
        if (k_hi > (int64_t)n / 2 - 1) k_hi = (int64_t)n / 2 - 1;
        double acc = 0.0;
        for (int64_t k = k_lo; k <= k_hi; k++) {
            double re = work[2 * k], im = work[2 * k + 1];
            acc += re * re + im * im;
        }
        power[b] += 2.0 * acc / ((double)n * w_sq);
    }
    return ALGAOPS_OK;
}

AlgaopsStatus features_band_rms(const int16_t *xyz, uint32_t n, int axis,
                                double g_per_lsb, double fs_hz,
                                const FeatureBand *bands, int nbands,
                                float *work, double *out_rms)
{
    if (!xyz || !bands || !work || !out_rms) return ALGAOPS_ERR_NULL_PTR;
    if (axis < 0 || axis > 3 || !(g_per_lsb > 0.0)) return ALGAOPS_ERR_RANGE;
    if (!features_spectral_supported(fs_hz, n, bands, nbands, 1u, NULL, 0)) {
        return ALGAOPS_ERR_RANGE;
    }
    double power[ALGA_MAX_BANDS] = { 0.0, 0.0, 0.0, 0.0 };
    int a0 = axis == 3 ? 0 : axis;
    int a1 = axis == 3 ? 2 : axis;
    for (int a = a0; a <= a1; a++) {
        AlgaopsStatus s = band_power_axis(xyz, n, a, g_per_lsb, fs_hz,
                                          bands, nbands, work, power);
        if (s != ALGAOPS_OK) return s;
    }
    for (int b = 0; b < nbands; b++) out_rms[b] = sqrt(power[b] > 0.0 ? power[b] : 0.0);
    return ALGAOPS_OK;
}
