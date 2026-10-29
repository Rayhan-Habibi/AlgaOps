/*
 * operating_mode.c — OFF / STARTUP / STEADY state machine.
 */

#include "../include/operating_mode.h"

#include <string.h>

void operating_mode_init(OperatingModeState *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->mode = ALGA_MODE_UNKNOWN;
}

bool operating_mode_hysteresis(double value, double on_thr, double off_thr,
                               bool prev_running)
{
    if (prev_running) return !(value < off_thr);
    return value > on_thr;
}

int operating_mode_update(OperatingModeState *s, const OperatingModeConfig *cfg,
                          const AlgaWindow *w, bool *changed,
                          uint64_t *time_in_mode)
{
    if (changed) *changed = false;
    if (time_in_mode) *time_in_mode = 0;
    if (!s || !cfg || !w) return ALGA_MODE_UNKNOWN;

    int mode;
    if (!cfg->enabled) {
        mode = ALGA_MODE_STEADY;
        s->running = true;
    } else {
        bool use_current = (w->presence & ALGA_HAS_CURRENT) != 0;
        bool prev = s->has_state ? s->running : false;
        bool running = use_current
            ? operating_mode_hysteresis(w->current_a, cfg->current_on_a,
                                        cfg->current_off_a, prev)
            : operating_mode_hysteresis(w->vibration_rms_g, cfg->vib_on_g,
                                        cfg->vib_off_g, prev);
        if (running && (!s->has_state || !s->running)) {
            /* Switch-on observed.  The very first window of a stream that is
             * already running is treated as switch-on too: without history
             * the classifier cannot prove the asset is past its startup. */
            s->on_since_ms = w->timestamp_ms;
        }
        s->running = running;
        if (!running) {
            mode = ALGA_MODE_OFF;
        } else {
            uint64_t on_for = w->timestamp_ms >= s->on_since_ms
                                  ? w->timestamp_ms - s->on_since_ms : 0;
            mode = (on_for < cfg->startup_ms) ? ALGA_MODE_STARTUP : ALGA_MODE_STEADY;
        }
    }

    if (!s->has_state || mode != s->mode) {
        if (s->has_state && changed) *changed = true;
        s->mode = mode;
        s->mode_since_ms = w->timestamp_ms;
    }
    s->has_state = true;
    if (time_in_mode && w->timestamp_ms >= s->mode_since_ms) {
        *time_in_mode = w->timestamp_ms - s->mode_since_ms;
    }
    return mode;
}

const char *operating_mode_str(int mode)
{
    switch (mode) {
        case ALGA_MODE_OFF:     return "OFF";
        case ALGA_MODE_STARTUP: return "STARTUP";
        case ALGA_MODE_STEADY:  return "STEADY";
        default:                return "UNKNOWN";
    }
}
