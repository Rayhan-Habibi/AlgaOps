/*
 * sample_window.h — window assembly for the sensor streams.
 *
 * Vibration: interleaved raw ADXL345 counts [x0,y0,z0,x1,...] are collected
 * until the window holds samples_expected samples (rate * window_ms / 1000).
 * Lost samples are never filled in: a FIFO overrun marks the window and lowers
 * samples_valid, which the gateway reports as INCOMPLETE_WINDOW.  Features come
 * from the C core (algaops-assurance/src/features.c), the same code the gateway
 * uses for raw-sample replays.
 *
 * Current: an exact AC-RMS accumulator (mean removed) for the CT signal.
 */

#ifndef SAMPLE_WINDOW_H
#define SAMPLE_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

#include "features.h"

typedef struct {
    int16_t  *xyz;               /* 3 * samples_expected counts, caller-owned */
    uint32_t  samples_expected;
    uint32_t  samples_valid;     /* collected in this window                  */
    uint32_t  samples_lost;      /* known lost (FIFO overrun)                 */
    bool      overrun;
} sample_window_t;

/** @p xyz must hold 3 * samples_expected int16 values; samples_expected >= 2. */
bool     sample_window_init(sample_window_t *w, int16_t *xyz, uint32_t samples_expected);
void     sample_window_reset(sample_window_t *w);

/** Append up to @p n samples; returns how many were consumed (stops when full). */
uint32_t sample_window_add(sample_window_t *w, const int16_t *xyz, uint32_t n);

/** Record a FIFO overrun: at least @p lost samples (>= 1) of this window are gone.
 *  They still count against the expected total, so a window never pretends to
 *  be complete. */
void     sample_window_mark_overrun(sample_window_t *w, uint32_t lost);

/** True once collected + lost samples reach samples_expected. */
bool     sample_window_full(const sample_window_t *w);

/** Time-domain features of the collected samples (ALGAOPS_OK on success). */
AlgaopsStatus sample_window_features(const sample_window_t *w, double g_per_lsb,
                                     VibFeatures *out);

/* ---- AC RMS of a scalar stream (CT current in mV) ----------------------- */

typedef struct {
    double   shift;      /* first sample: keeps the sums small and exact enough */
    double   sum;
    double   sum_sq;
    uint32_t n;
    uint32_t clipped;    /* samples at the ADC limits                           */
    uint32_t errors;     /* failed ADC reads                                    */
} ac_rms_acc_t;

void ac_rms_reset(ac_rms_acc_t *a);
void ac_rms_add(ac_rms_acc_t *a, double v, bool clipped);
void ac_rms_error(ac_rms_acc_t *a);

/** RMS of (v - mean) over the window; false when fewer than 2 samples. */
bool ac_rms_result(const ac_rms_acc_t *a, double *rms);

#endif /* SAMPLE_WINDOW_H */
