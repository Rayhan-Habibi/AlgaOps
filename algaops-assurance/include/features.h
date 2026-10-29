/**
 * @file features.h
 * @brief Vibration feature extraction shared by the ESP32 firmware and the core.
 *
 * Definitions (per axis, over one window of N raw accelerometer samples):
 *   mean   = (1/N) Σ x
 *   rms    = sqrt( (1/N) Σ (x - mean)² )      AC RMS: DC/gravity removed
 *   p2p    = max(x) - min(x)                    peak-to-peak (DC-independent)
 *   rms_mag = sqrt(rms_x² + rms_y² + rms_z²)   RMS of the AC acceleration vector
 *
 * rms is computed from exact 64-bit integer sums of the raw counts, so it is
 * deterministic across platforms and free of catastrophic cancellation:
 *   var = (N·Σx² − (Σx)²) / N²
 * For a zero-mean signal p2p >= rms always holds; TrustGuard relies on that.
 *
 * Spectral band RMS (optional) uses a periodic Hann window and a radix-2 FFT.
 * It is only computed when features_spectral_supported() confirms that the
 * sample rate, window length and frequency resolution support the bands.
 */

#ifndef FEATURES_H
#define FEATURES_H

#include "algaops_types.h"

#include <stddef.h>

/** Maximum samples per axis per window (keeps the int64 sums exact). */
#define FEATURES_MAX_SAMPLES 8192
/** Maximum FFT length. */
#define FEATURES_MAX_FFT 4096

typedef struct {
    uint32_t n;
    double   mean_g[3];
    double   rms_g[3];
    double   p2p_g[3];
    double   rms_mag_g;
    double   p2p_max_g;
    bool     frozen;     /**< all samples identical on every axis */
} VibFeatures;

typedef struct {
    double f_lo_hz;
    double f_hi_hz;
} FeatureBand;

/**
 * @brief Time-domain features from interleaved raw counts [x0,y0,z0,x1,...].
 * @param g_per_lsb  scale factor (ADXL345 full resolution: 0.0039 g/LSB typ.)
 * @return ALGAOPS_OK, ALGAOPS_ERR_NULL_PTR, or ALGAOPS_ERR_RANGE when
 *         n < 2, n > FEATURES_MAX_SAMPLES or the scale is not positive.
 */
AlgaopsStatus features_from_counts(const int16_t *xyz, uint32_t n,
                                   double g_per_lsb, VibFeatures *out);

/**
 * @brief Check that spectral bands are resolvable.
 *
 * Requirements: n is a power of two in [64, FEATURES_MAX_FFT]; every band
 * satisfies 0 < f_lo < f_hi <= 0.4·fs (anti-alias margin below Nyquist);
 * and each band spans at least @p min_bins_per_band FFT bins (df = fs/n).
 * When unsupported, a reason is written to @p why.
 */
bool features_spectral_supported(double fs_hz, uint32_t n,
                                 const FeatureBand *bands, int nbands,
                                 uint32_t min_bins_per_band,
                                 char *why, size_t why_len);

/**
 * @brief Band RMS (g) of the AC signal.  @p axis 0..2 selects one axis,
 *        3 combines all axes (vector band RMS).
 * @param work  scratch buffer of 2·n floats.
 */
AlgaopsStatus features_band_rms(const int16_t *xyz, uint32_t n, int axis,
                                double g_per_lsb, double fs_hz,
                                const FeatureBand *bands, int nbands,
                                float *work, double *out_rms);

/** In-place radix-2 complex FFT on interleaved (re, im) floats; n power of 2. */
AlgaopsStatus features_fft(float *data, uint32_t n);

#endif /* FEATURES_H */
