/*
 * cross_sensor.c — current/vibration running-state agreement.
 */

#include "../include/cross_sensor.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

void cross_sensor_init(CrossSensorState *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

CrossSensorVerdict cross_sensor_check(CrossSensorState *s,
                                      const CrossSensorConfig *cfg,
                                      const OperatingModeConfig *mode_cfg,
                                      const AlgaWindow *w,
                                      char *expl, size_t expl_len)
{
    if (expl && expl_len) expl[0] = '\0';
    if (!s || !cfg || !mode_cfg || !w || !cfg->enabled) return CROSS_OK;
    if (!(w->presence & ALGA_HAS_CURRENT)) return CROSS_OK;
    if (!isfinite(w->current_a) || !isfinite(w->vibration_rms_g)) return CROSS_OK;

    bool first = !s->initialized;
    bool vib_prev = first ? false : s->vib_running;
    bool cur_prev = first ? false : s->cur_running;
    s->vib_running = operating_mode_hysteresis(w->vibration_rms_g, mode_cfg->vib_on_g,
                                               mode_cfg->vib_off_g, vib_prev);
    s->cur_running = operating_mode_hysteresis(w->current_a, mode_cfg->current_on_a,
                                               mode_cfg->current_off_a, cur_prev);
    /* The first observation counts as a current transition (no history). */
    if (first || s->cur_running != cur_prev) s->cur_changed_ms = w->timestamp_ms;
    s->initialized = true;

    if (s->vib_running == s->cur_running) {
        s->disagreeing = false;
        s->disagree_since_ms = 0;
        return CROSS_OK;
    }
    if (!s->disagreeing) {
        s->disagreeing = true;
        s->disagree_since_ms = w->timestamp_ms;
    }
    uint64_t since_change = w->timestamp_ms >= s->cur_changed_ms
                                ? w->timestamp_ms - s->cur_changed_ms : 0;
    uint64_t for_ms = w->timestamp_ms >= s->disagree_since_ms
                          ? w->timestamp_ms - s->disagree_since_ms : 0;

    if (since_change < cfg->grace_ms) {
        if (expl && expl_len) {
            snprintf(expl, expl_len,
                     "running-state transient: current %.3f A (%s) and vibration %.4f g (%s) "
                     "disagree %.1f s after a current change (grace %.1f s) — equipment "
                     "health withheld", w->current_a, s->cur_running ? "ON" : "OFF",
                     w->vibration_rms_g, s->vib_running ? "ON" : "OFF",
                     (double)since_change / 1000.0, (double)cfg->grace_ms / 1000.0);
        }
        return CROSS_PENDING;
    }
    if (expl && expl_len) {
        if (s->cur_running) {
            snprintf(expl, expl_len,
                     "sensor disagreement for %.1f s: current %.3f A indicates ON but "
                     "vibration %.4f g is at OFF level — verify accelerometer mounting "
                     "and CT clamp before inspecting the pump",
                     (double)for_ms / 1000.0, w->current_a, w->vibration_rms_g);
        } else {
            snprintf(expl, expl_len,
                     "sensor disagreement for %.1f s: vibration %.4f g indicates ON but "
                     "current %.3f A is at OFF level — verify CT clamp and accelerometer "
                     "(vibration may come from neighbouring equipment)",
                     (double)for_ms / 1000.0, w->vibration_rms_g, w->current_a);
        }
    }
    return CROSS_FLAG;
}
