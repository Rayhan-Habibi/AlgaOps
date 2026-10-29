/*
 * trustguard.c — TrustGuard sensor-data integrity validator.
 *
 * Trust score algorithm (informational, 0–100):
 *   Start at 100.  Deduct points per flag category:
 *     MISSING_PACKET        −20
 *     DUPLICATE/OOO SEQ     −30
 *     BAD_TIMESTAMP         −25
 *     OUT_OF_RANGE          −20 each axis
 *     STUCK_SENSOR          −20 each axis
 *     NON_FINITE/MALFORMED  −50
 *   v2 flags: STALE −30, FUTURE_TIMESTAMP −25, REBOOT −10,
 *     INCOMPLETE_WINDOW −30, SELF_TEST_FAIL −40, SENSOR_FAULT −40,
 *     CROSS_SENSOR −30, CLOCK_UNSYNCED −25, INCONSISTENT −40,
 *     LOW_COMPLETENESS −30, SPIKE −30, STREAM_TIMEOUT −100.
 *   Clamped to [0, 100].
 *
 * Blocking flags (always force is_trusted = false, score notwithstanding):
 *   INTEGRITY_DUPLICATE_SEQ, INTEGRITY_OUT_OF_ORDER_SEQ,
 *   INTEGRITY_BAD_TIMESTAMP, INTEGRITY_NON_FINITE, INTEGRITY_MALFORMED,
 *   INTEGRITY_VIB_OUT_OF_RANGE, INTEGRITY_TEMP_OUT_OF_RANGE,
 *   INTEGRITY_VIB_STUCK, INTEGRITY_TEMP_STUCK, and every v2 flag except
 *   INTEGRITY_REBOOT.
 *
 * INTEGRITY_MISSING_PACKET and INTEGRITY_REBOOT are informational (deduct
 * from score) but are NOT themselves blocking flags for the packet that
 * arrives after the gap / reboot.  Sustained loss is handled at stream level
 * by INTEGRITY_LOW_COMPLETENESS (window_policy.c).
 */

#include "../include/trustguard.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/** Blocking flags that always override a trusted decision. */
static const IntegrityFlags BLOCKING_FLAGS =
    INTEGRITY_DUPLICATE_SEQ    |
    INTEGRITY_OUT_OF_ORDER_SEQ |
    INTEGRITY_BAD_TIMESTAMP    |
    INTEGRITY_NON_FINITE       |
    INTEGRITY_MALFORMED        |
    INTEGRITY_VIB_OUT_OF_RANGE |
    INTEGRITY_TEMP_OUT_OF_RANGE |
    INTEGRITY_VIB_STUCK        |
    INTEGRITY_TEMP_STUCK       |
    INTEGRITY_STALE            |
    INTEGRITY_FUTURE_TIMESTAMP |
    INTEGRITY_INCOMPLETE_WINDOW |
    INTEGRITY_SELF_TEST_FAIL   |
    INTEGRITY_SENSOR_FAULT     |
    INTEGRITY_CROSS_SENSOR     |
    INTEGRITY_CLOCK_UNSYNCED   |
    INTEGRITY_INCONSISTENT     |
    INTEGRITY_LOW_COMPLETENESS |
    INTEGRITY_SPIKE            |
    INTEGRITY_STREAM_TIMEOUT;

static const struct { IntegrityFlags bit; const char *name; int penalty; } FLAG_TABLE[] = {
    { INTEGRITY_MISSING_PACKET,    "MISSING_PACKET",    20 },
    { INTEGRITY_DUPLICATE_SEQ,     "DUPLICATE_SEQ",     30 },
    { INTEGRITY_OUT_OF_ORDER_SEQ,  "OUT_OF_ORDER_SEQ",  30 },
    { INTEGRITY_BAD_TIMESTAMP,     "BAD_TIMESTAMP",     25 },
    { INTEGRITY_VIB_OUT_OF_RANGE,  "VIB_OUT_OF_RANGE",  20 },
    { INTEGRITY_TEMP_OUT_OF_RANGE, "TEMP_OUT_OF_RANGE", 20 },
    { INTEGRITY_VIB_STUCK,         "VIB_STUCK",         20 },
    { INTEGRITY_TEMP_STUCK,        "TEMP_STUCK",        20 },
    { INTEGRITY_NON_FINITE,        "NON_FINITE",        50 },
    { INTEGRITY_MALFORMED,         "MALFORMED",         50 },
    { INTEGRITY_STALE,             "STALE",             30 },
    { INTEGRITY_FUTURE_TIMESTAMP,  "FUTURE_TIMESTAMP",  25 },
    { INTEGRITY_REBOOT,            "REBOOT",            10 },
    { INTEGRITY_INCOMPLETE_WINDOW, "INCOMPLETE_WINDOW", 30 },
    { INTEGRITY_SELF_TEST_FAIL,    "SELF_TEST_FAIL",    40 },
    { INTEGRITY_SENSOR_FAULT,      "SENSOR_FAULT",      40 },
    { INTEGRITY_CROSS_SENSOR,      "CROSS_SENSOR",      30 },
    { INTEGRITY_CLOCK_UNSYNCED,    "CLOCK_UNSYNCED",    25 },
    { INTEGRITY_INCONSISTENT,      "INCONSISTENT",      40 },
    { INTEGRITY_LOW_COMPLETENESS,  "LOW_COMPLETENESS",  30 },
    { INTEGRITY_SPIKE,             "SPIKE",             30 },
    { INTEGRITY_STREAM_TIMEOUT,    "STREAM_TIMEOUT",   100 },
};
#define FLAG_TABLE_LEN (sizeof(FLAG_TABLE) / sizeof(FLAG_TABLE[0]))

/** Append a formatted message to the explanation buffer. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 3, 0)))
#endif
static void append_explanation_v(char *buf, int buflen, const char *fmt, va_list ap)
{
    int used = (int)strlen(buf);
    int remaining = buflen - used;
    if (remaining <= 1) return;
    if (used > 0) {
        /* Insert separator between messages. */
        if (remaining > 3) {
            buf[used]     = ';';
            buf[used + 1] = ' ';
            buf[used + 2] = '\0';
            used     += 2;
            remaining -= 2;
        }
    }
    vsnprintf(buf + used, (size_t)remaining, fmt, ap);
}

/**
 * Push a value into the circular rolling window.
 * Returns the range (max - min) over the entire window.
 */
static double window_push_and_range(double *win, int *head, int *count,
                                     int capacity, double val)
{
    win[*head] = val;
    *head = (*head + 1) % capacity;
    if (*count < capacity) (*count)++;

    if (*count < 2) return 0.0;   /* not enough samples for range */

    double mn = win[0], mx = win[0];
    for (int i = 1; i < *count; i++) {
        if (win[i] < mn) mn = win[i];
        if (win[i] > mx) mx = win[i];
    }
    return mx - mn;
}

static int temp_capacity(const TrustGuardConfig *cfg)
{
    return cfg->stuck_temp_window_size > 0 ? cfg->stuck_temp_window_size
                                           : cfg->stuck_window_size;
}

static bool boot_seen(const TrustGuardState *s, uint32_t boot_id)
{
    for (int i = 0; i < s->boot_history_len; i++) {
        if (s->boot_history[i] == boot_id) return true;
    }
    return false;
}

static void boot_remember(TrustGuardState *s, uint32_t boot_id)
{
    if (boot_seen(s, boot_id)) return;
    if (s->boot_history_len < TRUSTGUARD_BOOT_HISTORY) {
        s->boot_history[s->boot_history_len++] = boot_id;
    } else {
        memmove(&s->boot_history[0], &s->boot_history[1],
                sizeof(s->boot_history[0]) * (TRUSTGUARD_BOOT_HISTORY - 1));
        s->boot_history[TRUSTGUARD_BOOT_HISTORY - 1] = boot_id;
    }
}

/* =========================================================================
 * Flag helpers (public)
 * ========================================================================= */

int trustguard_flag_penalty(IntegrityFlags flag)
{
    for (size_t i = 0; i < FLAG_TABLE_LEN; i++) {
        if (FLAG_TABLE[i].bit == flag) return FLAG_TABLE[i].penalty;
    }
    return 0;
}

IntegrityFlags trustguard_blocking_flags(void)
{
    return BLOCKING_FLAGS;
}

const char *trustguard_flag_name(IntegrityFlags flag)
{
    for (size_t i = 0; i < FLAG_TABLE_LEN; i++) {
        if (FLAG_TABLE[i].bit == flag) return FLAG_TABLE[i].name;
    }
    return "UNKNOWN";
}

void trustguard_add_flag(TrustResult *result, IntegrityFlags flag,
                         const char *fmt, ...)
{
    if (!result) return;
    if (!(result->flags & flag)) {
        result->trust_score -= trustguard_flag_penalty(flag);
    }
    result->flags |= flag;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        append_explanation_v(result->explanation, ALGAOPS_EXPLANATION_LEN, fmt, ap);
        va_end(ap);
    }
}

void trustguard_finalize(const TrustGuardConfig *config, TrustResult *result)
{
    if (!config || !result) return;
    if (result->trust_score < 0)   result->trust_score = 0;
    if (result->trust_score > 100) result->trust_score = 100;

    bool has_blocking = (result->flags & BLOCKING_FLAGS) != 0;
    bool score_ok     = (result->trust_score >= config->trust_score_threshold);
    result->is_trusted = (!has_blocking && score_ok);

    /* Set friendly explanation for clean records. */
    if (result->flags == INTEGRITY_OK) {
        strncpy(result->explanation, "OK", ALGAOPS_EXPLANATION_LEN - 1);
        result->explanation[ALGAOPS_EXPLANATION_LEN - 1] = '\0';
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

AlgaopsStatus trustguard_init(TrustGuardState        *state,
                               const TrustGuardConfig *config)
{
    if (!state || !config) return ALGAOPS_ERR_NULL_PTR;
    if (config->stuck_window_size < 1 ||
        config->stuck_window_size > TRUSTGUARD_MAX_WINDOW) {
        fprintf(stderr,
                "[trustguard] stuck_window_size %d out of range [1,%d]\n",
                config->stuck_window_size, TRUSTGUARD_MAX_WINDOW);
        return ALGAOPS_ERR_GENERIC;
    }
    if (config->stuck_temp_window_size < 0 ||
        config->stuck_temp_window_size > TRUSTGUARD_MAX_WINDOW) {
        fprintf(stderr,
                "[trustguard] stuck_temp_window_size %d out of range [0,%d]\n",
                config->stuck_temp_window_size, TRUSTGUARD_MAX_WINDOW);
        return ALGAOPS_ERR_GENERIC;
    }

    memset(state, 0, sizeof(*state));
    state->config = *config;
    return trustguard_reset(state);
}

AlgaopsStatus trustguard_reset_history(TrustGuardState *state)
{
    if (!state) return ALGAOPS_ERR_NULL_PTR;
    state->window_count      = 0;
    state->window_head       = 0;
    state->temp_count        = 0;
    state->temp_head         = 0;
    state->has_prev_temp     = false;
    state->prev_temp_c       = 0.0;
    state->prev_temp_ts_ms   = 0;
    memset(state->vib_window,  0, sizeof(state->vib_window));
    memset(state->temp_window, 0, sizeof(state->temp_window));
    return ALGAOPS_OK;
}

AlgaopsStatus trustguard_reset(TrustGuardState *state)
{
    if (!state) return ALGAOPS_ERR_NULL_PTR;

    state->has_previous      = false;
    state->prev_timestamp_ms = 0;
    state->prev_sequence_id  = 0;
    state->prev_boot_id      = 0;
    state->boot_history_len  = 0;
    memset(state->boot_history, 0, sizeof(state->boot_history));
    state->last_missing_count = 0;
    state->last_reboot        = false;
    state->last_forward       = false;
    return trustguard_reset_history(state);
}

AlgaopsStatus trustguard_validate(TrustGuardState  *state,
                                   const AlgaRecord *record,
                                   TrustResult      *result)
{
    if (!state || !record || !result) return ALGAOPS_ERR_NULL_PTR;

    /* A v1 record is a window with no optional fields present. */
    AlgaWindow w;
    memset(&w, 0, sizeof(w));
    w.timestamp_ms    = record->timestamp_ms;
    w.sequence_id     = record->sequence_id;
    w.vibration_rms_g = record->vibration_rms_g;
    w.temperature_c   = record->temperature_c;
    return trustguard_validate_window(state, &w, result);
}

AlgaopsStatus trustguard_validate_window(TrustGuardState  *state,
                                          const AlgaWindow *w,
                                          TrustResult      *result)
{
    if (!state || !w || !result) return ALGAOPS_ERR_NULL_PTR;

    /* Zero-initialise result. */
    memset(result, 0, sizeof(*result));
    result->trust_score = 100;
    result->flags       = INTEGRITY_OK;
    result->explanation[0] = '\0';

    const TrustGuardConfig *cfg = &state->config;
    const uint32_t has = w->presence;

    state->last_missing_count = 0;
    state->last_reboot        = false;
    state->last_forward       = false;

    /* ------------------------------------------------------------------ */
    /* 1. Non-finite values                                                */
    /* ------------------------------------------------------------------ */
    bool non_finite = !isfinite(w->vibration_rms_g) || !isfinite(w->temperature_c);
    for (int i = 0; i < 3; i++) {
        if ((has & ALGA_HAS_AXES) && !isfinite(w->vib_rms_axis_g[i])) non_finite = true;
        if ((has & ALGA_HAS_P2P)  && !isfinite(w->vib_p2p_axis_g[i])) non_finite = true;
    }
    if ((has & ALGA_HAS_CURRENT) && !isfinite(w->current_a)) non_finite = true;
    if (has & ALGA_HAS_BANDS) {
        int nb = w->n_bands < 0 ? 0 : (w->n_bands > ALGA_MAX_BANDS ? ALGA_MAX_BANDS : w->n_bands);
        for (int i = 0; i < nb; i++) {
            if (!isfinite(w->band_rms_g[i])) non_finite = true;
        }
    }
    if (non_finite) {
        trustguard_add_flag(result, INTEGRITY_NON_FINITE,
                            "non-finite numeric value detected");
    }

    /* ------------------------------------------------------------------ */
    /* 2. Boot / session identity (v2; v1 records always carry boot_id 0)  */
    /* ------------------------------------------------------------------ */
    bool superseded_session = false;
    if (state->has_previous && w->boot_id != state->prev_boot_id) {
        if (boot_seen(state, w->boot_id)) {
            superseded_session = true;
            trustguard_add_flag(result, INTEGRITY_OUT_OF_ORDER_SEQ,
                                "window from superseded boot session 0x%08x "
                                "(current 0x%08x)",
                                (unsigned)w->boot_id, (unsigned)state->prev_boot_id);
        } else {
            state->last_reboot = true;
            trustguard_add_flag(result, INTEGRITY_REBOOT,
                                "node reboot detected: boot_id 0x%08x -> 0x%08x; "
                                "sequence tracking and value history restarted",
                                (unsigned)state->prev_boot_id, (unsigned)w->boot_id);
            trustguard_reset_history(state);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 3. Sequence checks (serial-number arithmetic, RFC 1982)             */
    /* ------------------------------------------------------------------ */
    if (state->has_previous && !state->last_reboot && !superseded_session) {
        uint32_t diff     = w->sequence_id - state->prev_sequence_id;
        uint32_t expected = state->prev_sequence_id + 1u;

        if (diff == 0u) {
            trustguard_add_flag(result, INTEGRITY_DUPLICATE_SEQ,
                                "duplicate sequence_id %u",
                                (unsigned)w->sequence_id);
        } else if (diff >= 0x80000000u) {
            trustguard_add_flag(result, INTEGRITY_OUT_OF_ORDER_SEQ,
                                "out-of-order sequence_id %u (prev %u)",
                                (unsigned)w->sequence_id,
                                (unsigned)state->prev_sequence_id);
        } else if (diff > 1u) {
            /* Missing packets detected at next arrival. */
            uint32_t gap = diff - 1u;
            state->last_missing_count = gap;
            trustguard_add_flag(result, INTEGRITY_MISSING_PACKET,
                                "%u missing packet(s) before sequence_id %u "
                                "(expected %u)",
                                (unsigned)gap, (unsigned)w->sequence_id,
                                (unsigned)expected);
            if (cfg->history_reset_gap > 0 && gap >= cfg->history_reset_gap) {
                trustguard_reset_history(state);
                trustguard_add_flag(result, INTEGRITY_MISSING_PACKET,
                                    "value history restarted after gap of %u windows",
                                    (unsigned)gap);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* 4. Timestamp checks                                                 */
    /* ------------------------------------------------------------------ */
    if (state->has_previous) {
        if (w->timestamp_ms <= state->prev_timestamp_ms) {
            trustguard_add_flag(result, INTEGRITY_BAD_TIMESTAMP,
                                "non-monotonic timestamp: %llu <= prev %llu",
                                (unsigned long long)w->timestamp_ms,
                                (unsigned long long)state->prev_timestamp_ms);
        }
    }
    if (has & ALGA_HAS_TIMING) {
        if (w->t_start_ms > w->timestamp_ms) {
            trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                "window start %llu after window end %llu",
                                (unsigned long long)w->t_start_ms,
                                (unsigned long long)w->timestamp_ms);
        } else if (cfg->window_timing_tolerance_ms > 0) {
            uint64_t dur = w->timestamp_ms - w->t_start_ms;
            uint64_t nominal = w->window_ms;
            uint64_t dev = dur > nominal ? dur - nominal : nominal - dur;
            if (dev > cfg->window_timing_tolerance_ms) {
                trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                    "window duration %llu ms differs from nominal %llu ms",
                                    (unsigned long long)dur,
                                    (unsigned long long)nominal);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* 5. Freshness, clock skew, clock synchronisation                     */
    /* ------------------------------------------------------------------ */
    if (has & ALGA_HAS_RECEIVED) {
        if (cfg->max_staleness_ms > 0 && w->received_ms > w->timestamp_ms &&
            (w->received_ms - w->timestamp_ms) > cfg->max_staleness_ms) {
            trustguard_add_flag(result, INTEGRITY_STALE,
                                "window delivered late: age %.1f s > %.1f s "
                                "(kept for history, not used for live diagnosis)",
                                (double)(w->received_ms - w->timestamp_ms) / 1000.0,
                                (double)cfg->max_staleness_ms / 1000.0);
        }
        if (cfg->max_future_skew_ms > 0 && w->timestamp_ms > w->received_ms &&
            (w->timestamp_ms - w->received_ms) > cfg->max_future_skew_ms) {
            trustguard_add_flag(result, INTEGRITY_FUTURE_TIMESTAMP,
                                "timestamp %.1f s ahead of gateway clock (max %.1f s)",
                                (double)(w->timestamp_ms - w->received_ms) / 1000.0,
                                (double)cfg->max_future_skew_ms / 1000.0);
        }
    }
    if (cfg->require_time_sync && (has & ALGA_HAS_TIME_SYNC) && !w->time_synced) {
        trustguard_add_flag(result, INTEGRITY_CLOCK_UNSYNCED,
                            "node clock not synchronised at capture");
    }

    /* ------------------------------------------------------------------ */
    /* 6. Range checks (only if values are finite)                         */
    /* ------------------------------------------------------------------ */
    if (!(result->flags & INTEGRITY_NON_FINITE)) {
        if (w->vibration_rms_g < cfg->vib_min_g ||
            w->vibration_rms_g > cfg->vib_max_g) {
            trustguard_add_flag(result, INTEGRITY_VIB_OUT_OF_RANGE,
                                "vibration %.4f g outside [%.4f, %.4f] g",
                                w->vibration_rms_g,
                                cfg->vib_min_g, cfg->vib_max_g);
        }
        if (w->temperature_c < cfg->temp_min_c ||
            w->temperature_c > cfg->temp_max_c) {
            trustguard_add_flag(result, INTEGRITY_TEMP_OUT_OF_RANGE,
                                "temperature %.2f °C outside [%.2f, %.2f] °C",
                                w->temperature_c,
                                cfg->temp_min_c, cfg->temp_max_c);
        }
        if ((has & ALGA_HAS_CURRENT) &&
            (w->current_a < cfg->current_min_a || w->current_a > cfg->current_max_a)) {
            trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                "current %.3f A outside plausible [%.3f, %.3f] A",
                                w->current_a, cfg->current_min_a, cfg->current_max_a);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 7. Sample counts and in-window completeness                         */
    /* ------------------------------------------------------------------ */
    if (has & ALGA_HAS_SAMPLE_COUNTS) {
        if (w->samples_expected == 0u || w->samples_valid > w->samples_expected) {
            trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                "invalid sample counts: %u valid of %u expected",
                                (unsigned)w->samples_valid,
                                (unsigned)w->samples_expected);
        } else if (cfg->min_window_completeness > 0.0) {
            double ratio = (double)w->samples_valid / (double)w->samples_expected;
            if (ratio < cfg->min_window_completeness) {
                trustguard_add_flag(result, INTEGRITY_INCOMPLETE_WINDOW,
                                    "window incomplete: %u/%u samples valid "
                                    "(%.0f%% < %.0f%%)",
                                    (unsigned)w->samples_valid,
                                    (unsigned)w->samples_expected,
                                    ratio * 100.0,
                                    cfg->min_window_completeness * 100.0);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* 8. Internal consistency of vibration features                       */
    /* ------------------------------------------------------------------ */
    if (!(result->flags & INTEGRITY_NON_FINITE)) {
        const double tol = cfg->consistency_rel_tol > 0.0 ? cfg->consistency_rel_tol : 0.0;
        if (has & ALGA_HAS_AXES) {
            double sum_sq = 0.0;
            bool negative = false;
            for (int i = 0; i < 3; i++) {
                if (w->vib_rms_axis_g[i] < 0.0) negative = true;
                sum_sq += w->vib_rms_axis_g[i] * w->vib_rms_axis_g[i];
            }
            double mag_sq = w->vibration_rms_g * w->vibration_rms_g;
            double scale  = fmax(fmax(mag_sq, sum_sq), 1e-12);
            if (negative) {
                trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                    "negative per-axis vibration RMS");
            } else if (fabs(mag_sq - sum_sq) > tol * scale + 1e-9) {
                trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                    "vibration magnitude %.4f g inconsistent with "
                                    "axes (expected %.4f g)",
                                    w->vibration_rms_g, sqrt(sum_sq));
            }
        }
        if (has & ALGA_HAS_P2P) {
            static const char AXIS[3] = { 'x', 'y', 'z' };
            for (int i = 0; i < 3; i++) {
                if (w->vib_p2p_axis_g[i] < 0.0) {
                    trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                        "negative peak-to-peak on axis %c", AXIS[i]);
                    break;
                }
                if ((has & ALGA_HAS_AXES) &&
                    w->vib_p2p_axis_g[i] + 1e-9 < w->vib_rms_axis_g[i] * (1.0 - tol)) {
                    trustguard_add_flag(result, INTEGRITY_INCONSISTENT,
                                        "peak-to-peak %.4f g below RMS %.4f g on axis %c",
                                        w->vib_p2p_axis_g[i], w->vib_rms_axis_g[i],
                                        AXIS[i]);
                    break;
                }
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* 9. Self-test and node-reported faults                               */
    /* ------------------------------------------------------------------ */
    if ((has & ALGA_HAS_SELF_TEST) && w->self_test == ALGA_SELF_TEST_FAIL) {
        trustguard_add_flag(result, INTEGRITY_SELF_TEST_FAIL,
                            "accelerometer self-test FAILED");
    }
    if ((has & ALGA_HAS_NODE_FAULTS) && w->node_fault_flags != 0u) {
        char faults[160];
        trustguard_node_faults_to_str(w->node_fault_flags, faults, sizeof(faults));
        trustguard_add_flag(result, INTEGRITY_SENSOR_FAULT,
                            "node reported sensor fault(s): %s", faults);
    }

    /* ------------------------------------------------------------------ */
    /* 10. Temperature rate of change (spike plausibility)                 */
    /* ------------------------------------------------------------------ */
    const IntegrityFlags seq_reject = INTEGRITY_DUPLICATE_SEQ | INTEGRITY_OUT_OF_ORDER_SEQ;
    if (cfg->max_temp_rate_c_per_s > 0.0 && state->has_prev_temp &&
        !(result->flags & (INTEGRITY_NON_FINITE | seq_reject)) &&
        w->timestamp_ms > state->prev_temp_ts_ms) {
        double dt_s = (double)(w->timestamp_ms - state->prev_temp_ts_ms) / 1000.0;
        double dT   = fabs(w->temperature_c - state->prev_temp_c);
        double rate = dT / dt_s;
        if (rate > cfg->max_temp_rate_c_per_s) {
            trustguard_add_flag(result, INTEGRITY_SPIKE,
                                "implausible temperature change %.2f °C in %.1f s "
                                "(%.2f °C/s > %.2f °C/s)",
                                dT, dt_s, rate, cfg->max_temp_rate_c_per_s);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 11. Stuck-sensor detection (rolling window, only for finite values) */
    /* ------------------------------------------------------------------ */
    if (!(result->flags & INTEGRITY_NON_FINITE) &&
        !(result->flags & INTEGRITY_DUPLICATE_SEQ) &&
        !(result->flags & INTEGRITY_OUT_OF_ORDER_SEQ)) {

        int vcap = cfg->stuck_window_size;
        int tcap = temp_capacity(cfg);

        double vib_range = window_push_and_range(
            state->vib_window, &state->window_head,
            &state->window_count, vcap, w->vibration_rms_g);
        double temp_range = window_push_and_range(
            state->temp_window, &state->temp_head,
            &state->temp_count, tcap, w->temperature_c);

        /* Only flag stuck if window is full. */
        if (state->window_count >= vcap && vib_range < cfg->stuck_vib_threshold_g) {
            trustguard_add_flag(result, INTEGRITY_VIB_STUCK,
                                "vibration stuck: range %.6f g < "
                                "threshold %.6f g over %d samples",
                                vib_range, cfg->stuck_vib_threshold_g, vcap);
        }
        if (state->temp_count >= tcap && temp_range < cfg->stuck_temp_threshold_c) {
            trustguard_add_flag(result, INTEGRITY_TEMP_STUCK,
                                "temperature stuck: range %.6f °C < "
                                "threshold %.6f °C over %d samples",
                                temp_range, cfg->stuck_temp_threshold_c, tcap);
        }

        /* A live accelerometer always shows noise: identical raw samples on
         * every axis inside one window means the output is frozen. */
        if ((has & ALGA_HAS_P2P) && cfg->flag_zero_p2p_as_stuck &&
            w->vib_p2p_axis_g[0] == 0.0 && w->vib_p2p_axis_g[1] == 0.0 &&
            w->vib_p2p_axis_g[2] == 0.0 &&
            (!(has & ALGA_HAS_SAMPLE_COUNTS) || w->samples_valid > 1u)) {
            trustguard_add_flag(result, INTEGRITY_VIB_STUCK,
                                "accelerometer output frozen: zero peak-to-peak on "
                                "all axes within window");
        }
    }

    /* ------------------------------------------------------------------ */
    /* 12. Clamp score and compute trust decision                          */
    /* ------------------------------------------------------------------ */
    trustguard_finalize(cfg, result);

    /* ------------------------------------------------------------------ */
    /* 13. Update state for next call                                      */
    /*    Advance sequence/timestamp tracking whenever records move strictly*/
    /*    forward, ensuring transport sequence tracking remains coherent   */
    /*    even if physical sensor readings (e.g. stuck sensor) fail, and   */
    /*    preventing duplicate missing packet warnings on subsequent steps.*/
    /* ------------------------------------------------------------------ */
    if (!state->has_previous) {
        state->has_previous      = true;
        state->prev_sequence_id  = w->sequence_id;
        state->prev_timestamp_ms = w->timestamp_ms;
        state->prev_boot_id      = w->boot_id;
        boot_remember(state, w->boot_id);
        state->last_forward      = true;
    } else if (state->last_reboot) {
        state->prev_boot_id      = w->boot_id;
        boot_remember(state, w->boot_id);
        state->prev_sequence_id  = w->sequence_id;
        if (w->timestamp_ms > state->prev_timestamp_ms &&
            !(result->flags & INTEGRITY_FUTURE_TIMESTAMP)) {
            state->prev_timestamp_ms = w->timestamp_ms;
        }
        state->last_forward      = true;
    } else if (!superseded_session) {
        uint32_t diff = w->sequence_id - state->prev_sequence_id;
        if (diff != 0u && diff < 0x80000000u) {
            state->prev_sequence_id = w->sequence_id;
            state->last_forward     = true;
        }
        /* A future-dated window must not become the monotonicity reference,
         * otherwise every correct window after it would look regressive. */
        if (w->timestamp_ms > state->prev_timestamp_ms &&
            !(result->flags & INTEGRITY_FUTURE_TIMESTAMP)) {
            state->prev_timestamp_ms = w->timestamp_ms;
        }
    }

    if (isfinite(w->temperature_c) &&
        !(result->flags & (INTEGRITY_NON_FINITE | seq_reject | INTEGRITY_SPIKE))) {
        state->has_prev_temp   = true;
        state->prev_temp_c     = w->temperature_c;
        state->prev_temp_ts_ms = w->timestamp_ms;
    }

    return ALGAOPS_OK;
}

void trustguard_flags_to_str(IntegrityFlags flags, char *buf, int buflen)
{
    if (!buf || buflen < 1) return;
    buf[0] = '\0';

    if (flags == INTEGRITY_OK) {
        snprintf(buf, (size_t)buflen, "INTEGRITY_OK");
        return;
    }

    bool first = true;
    for (size_t i = 0; i < FLAG_TABLE_LEN; i++) {
        if (flags & FLAG_TABLE[i].bit) {
            if (!first) strncat(buf, "|", (size_t)(buflen - (int)strlen(buf) - 1));
            strncat(buf, FLAG_TABLE[i].name, (size_t)(buflen - (int)strlen(buf) - 1));
            first = false;
        }
    }
}

void trustguard_node_faults_to_str(uint32_t faults, char *buf, size_t buflen)
{
    static const char *NAMES[ALGA_NODE_FAULT_COUNT] = {
        "ACCEL_BUS", "ACCEL_OVERRUN", "ACCEL_MISSING", "TEMP_CRC",
        "TEMP_MISSING", "TEMP_POR", "CURRENT_ADC", "CURRENT_CLIP"
    };
    if (!buf || buflen == 0) return;
    buf[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < ALGA_NODE_FAULT_COUNT; i++) {
        if (!(faults & (1u << i))) continue;
        int n = snprintf(buf + used, buflen - used, "%s%s", used ? "|" : "", NAMES[i]);
        if (n < 0 || (size_t)n >= buflen - used) return;
        used += (size_t)n;
    }
    uint32_t unknown = faults & ~((1u << ALGA_NODE_FAULT_COUNT) - 1u);
    if (unknown && used < buflen) {
        snprintf(buf + used, buflen - used, "%sUNKNOWN(0x%x)", used ? "|" : "",
                 (unsigned)unknown);
    }
}
