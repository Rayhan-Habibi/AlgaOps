/**
 * @file operating_mode.h
 * @brief OFF / STARTUP / STEADY classification of the monitored pump/blower.
 *
 * Running state comes from RMS current when the window carries it, otherwise
 * from vibration RMS, each with hysteresis (on threshold > off threshold).
 * After an OFF->ON transition the mode is STARTUP for startup_ms of node time,
 * then STEADY.  Only trusted windows advance the state machine; the engine
 * reports ALGA_MODE_UNKNOWN for untrusted windows.
 *
 * With enabled == false the classifier always returns STEADY, which is the
 * single-mode behaviour of the original v1 core.
 */

#ifndef OPERATING_MODE_H
#define OPERATING_MODE_H

#include "algaops_types.h"

typedef struct {
    bool     enabled;
    double   vib_on_g;        /**< vibration RMS above which the asset runs   */
    double   vib_off_g;       /**< vibration RMS below which the asset is off */
    double   current_on_a;    /**< current above which the asset runs         */
    double   current_off_a;   /**< current below which the asset is off       */
    uint32_t startup_ms;      /**< STARTUP duration after switch-on           */
} OperatingModeConfig;

static inline OperatingModeConfig operating_mode_default_config(void)
{
    OperatingModeConfig c;
    c.enabled       = false;  /* v1 behaviour: single STEADY mode */
    c.vib_on_g      = 0.10;
    c.vib_off_g     = 0.05;
    c.current_on_a  = 0.50;
    c.current_off_a = 0.20;
    c.startup_ms    = 30000;
    return c;
}

typedef struct {
    bool     has_state;
    bool     running;
    uint64_t on_since_ms;
    int      mode;            /**< AlgaMode of the last trusted window */
    uint64_t mode_since_ms;
} OperatingModeState;

void operating_mode_init(OperatingModeState *s);

/**
 * @brief Hysteresis helper shared with cross_sensor.c.
 * @return new running state given value and previous state.
 */
bool operating_mode_hysteresis(double value, double on_thr, double off_thr,
                               bool prev_running);

/**
 * @brief Advance the classifier with one trusted window.
 * @param changed      optional output: true when the mode differs from the
 *                     previous trusted window (false for the first window).
 * @param time_in_mode optional output: ms spent in the returned mode.
 * @return AlgaMode for this window.
 */
int operating_mode_update(OperatingModeState *s, const OperatingModeConfig *cfg,
                          const AlgaWindow *w, bool *changed,
                          uint64_t *time_in_mode);

/** Constant label for an AlgaMode value. */
const char *operating_mode_str(int mode);

#endif /* OPERATING_MODE_H */
