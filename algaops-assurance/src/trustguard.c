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
 *   Clamped to [0, 100].
 *
 * Blocking flags (always force is_trusted = false, score notwithstanding):
 *   INTEGRITY_DUPLICATE_SEQ, INTEGRITY_OUT_OF_ORDER_SEQ,
 *   INTEGRITY_BAD_TIMESTAMP, INTEGRITY_NON_FINITE, INTEGRITY_MALFORMED,
 *   INTEGRITY_VIB_OUT_OF_RANGE, INTEGRITY_TEMP_OUT_OF_RANGE,
 *   INTEGRITY_VIB_STUCK, INTEGRITY_TEMP_STUCK.
 *
 * INTEGRITY_MISSING_PACKET is informational (deducts from score) but is NOT
 * itself a blocking flag for the packet that arrives after the gap.
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
    INTEGRITY_TEMP_STUCK;

/** Append a formatted message to the explanation buffer. */
static void append_explanation(char *buf, int buflen, const char *fmt, ...)
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
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, (size_t)remaining, fmt, ap);
    va_end(ap);
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

    memset(state, 0, sizeof(*state));
    state->config = *config;
    return trustguard_reset(state);
}

AlgaopsStatus trustguard_reset(TrustGuardState *state)
{
    if (!state) return ALGAOPS_ERR_NULL_PTR;

    state->has_previous      = false;
    state->prev_timestamp_ms = 0;
    state->prev_sequence_id  = 0;
    state->window_count      = 0;
    state->window_head       = 0;
    memset(state->vib_window,  0, sizeof(state->vib_window));
    memset(state->temp_window, 0, sizeof(state->temp_window));
    return ALGAOPS_OK;
}

AlgaopsStatus trustguard_validate(TrustGuardState  *state,
                                   const AlgaRecord *record,
                                   TrustResult      *result)
{
    if (!state || !record || !result) return ALGAOPS_ERR_NULL_PTR;

    /* Zero-initialise result. */
    memset(result, 0, sizeof(*result));
    result->trust_score = 100;
    result->flags       = INTEGRITY_OK;
    result->explanation[0] = '\0';

    const TrustGuardConfig *cfg = &state->config;

    /* ------------------------------------------------------------------ */
    /* 1. Non-finite values                                                */
    /* ------------------------------------------------------------------ */
    if (!isfinite(record->vibration_rms_g) ||
        !isfinite(record->temperature_c)) {
        result->flags |= INTEGRITY_NON_FINITE;
        result->trust_score -= 50;
        append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                           "non-finite numeric value detected");
    }

    /* ------------------------------------------------------------------ */
    /* 2. Sequence checks                                                  */
    /* ------------------------------------------------------------------ */
    if (state->has_previous) {
        uint32_t expected = state->prev_sequence_id + 1;

        if (record->sequence_id == state->prev_sequence_id) {
            result->flags |= INTEGRITY_DUPLICATE_SEQ;
            result->trust_score -= 30;
            append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                               "duplicate sequence_id %u",
                               record->sequence_id);
        } else if (record->sequence_id < state->prev_sequence_id) {
            result->flags |= INTEGRITY_OUT_OF_ORDER_SEQ;
            result->trust_score -= 30;
            append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                               "out-of-order sequence_id %u (prev %u)",
                               record->sequence_id,
                               state->prev_sequence_id);
        } else if (record->sequence_id > expected) {
            /* Missing packets detected at next arrival. */
            uint32_t gap = record->sequence_id - expected;
            result->flags |= INTEGRITY_MISSING_PACKET;
            result->trust_score -= 20;
            append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                               "%u missing packet(s) before sequence_id %u "
                               "(expected %u)",
                               gap, record->sequence_id, expected);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 3. Timestamp checks                                                 */
    /* ------------------------------------------------------------------ */
    if (state->has_previous) {
        if (record->timestamp_ms <= state->prev_timestamp_ms) {
            result->flags |= INTEGRITY_BAD_TIMESTAMP;
            result->trust_score -= 25;
            append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                               "non-monotonic timestamp: %llu <= prev %llu",
                               (unsigned long long)record->timestamp_ms,
                               (unsigned long long)state->prev_timestamp_ms);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 4. Range checks (only if values are finite)                         */
    /* ------------------------------------------------------------------ */
    if (!(result->flags & INTEGRITY_NON_FINITE)) {
        if (record->vibration_rms_g < cfg->vib_min_g ||
            record->vibration_rms_g > cfg->vib_max_g) {
            result->flags |= INTEGRITY_VIB_OUT_OF_RANGE;
            result->trust_score -= 20;
            append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                               "vibration %.4f g outside [%.4f, %.4f] g",
                               record->vibration_rms_g,
                               cfg->vib_min_g, cfg->vib_max_g);
        }
        if (record->temperature_c < cfg->temp_min_c ||
            record->temperature_c > cfg->temp_max_c) {
            result->flags |= INTEGRITY_TEMP_OUT_OF_RANGE;
            result->trust_score -= 20;
            append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                               "temperature %.2f °C outside [%.2f, %.2f] °C",
                               record->temperature_c,
                               cfg->temp_min_c, cfg->temp_max_c);
        }
    }

    /* ------------------------------------------------------------------ */
    /* 5. Stuck-sensor detection (rolling window, only for finite values)  */
    /* ------------------------------------------------------------------ */
    if (!(result->flags & INTEGRITY_NON_FINITE) &&
        !(result->flags & INTEGRITY_DUPLICATE_SEQ) &&
        !(result->flags & INTEGRITY_OUT_OF_ORDER_SEQ)) {

        int cap = cfg->stuck_window_size;

        double vib_range = window_push_and_range(
            state->vib_window, &state->window_head,
            &state->window_count, cap, record->vibration_rms_g);

        /* temp window uses a separate counter; keep in sync by using the
         * same head/count after the vib push.  We need a second call with
         * a separate head so use a local index trick. */
        /* Simple approach: share window_head and window_count between both
         * channels, storing each channel in its own array but always
         * advancing together (one sample = one push per channel). */
        /* NOTE: The vib push already advanced head and count.  Roll back
         * to push temp at the SAME position.  We accomplish this by pushing
         * temp with (head-1) before the head was advanced. */
        /* Simpler: keep separate state for each channel. */
        /* Implementation: use a second local circular window for temp. */

        /* Actually the simplest correct approach: use a dedicated helper
         * that takes its own head/count pair. */
        /* Let's maintain temp separately. We will use (window_head - 1) for
         * temp since vib already advanced it.  To avoid this coupling, use
         * a dedicated temp_head and temp_count that mirror window_head and
         * window_count. */

        /* Recalculate: push temp at the location BEFORE vib advanced the
         * head. The position used for vib was (old_head). After the push,
         * window_head = old_head + 1. So temp should go to old_head = 
         * (window_head - 1 + cap) % cap. */
        int temp_pos = (state->window_head - 1 + cap) % cap;
        state->temp_window[temp_pos] = record->temperature_c;

        /* Compute temp range over same count as vib. */
        double temp_range = 0.0;
        if (state->window_count >= 2) {
            double mn = state->temp_window[0], mx = state->temp_window[0];
            for (int i = 1; i < state->window_count; i++) {
                if (state->temp_window[i] < mn) mn = state->temp_window[i];
                if (state->temp_window[i] > mx) mx = state->temp_window[i];
            }
            temp_range = mx - mn;
        }

        /* Only flag stuck if window is full. */
        if (state->window_count >= cap) {
            if (vib_range < cfg->stuck_vib_threshold_g) {
                result->flags |= INTEGRITY_VIB_STUCK;
                result->trust_score -= 20;
                append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                                   "vibration stuck: range %.6f g < "
                                   "threshold %.6f g over %d samples",
                                   vib_range,
                                   cfg->stuck_vib_threshold_g, cap);
            }
            if (temp_range < cfg->stuck_temp_threshold_c) {
                result->flags |= INTEGRITY_TEMP_STUCK;
                result->trust_score -= 20;
                append_explanation(result->explanation, ALGAOPS_EXPLANATION_LEN,
                                   "temperature stuck: range %.6f °C < "
                                   "threshold %.6f °C over %d samples",
                                   temp_range,
                                   cfg->stuck_temp_threshold_c, cap);
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* 6. Clamp score and compute trust decision                           */
    /* ------------------------------------------------------------------ */
    if (result->trust_score < 0)   result->trust_score = 0;
    if (result->trust_score > 100) result->trust_score = 100;

    bool has_blocking = (result->flags & BLOCKING_FLAGS) != 0;
    bool score_ok     = (result->trust_score >= cfg->trust_score_threshold);
    result->is_trusted = (!has_blocking && score_ok);

    /* Set friendly explanation for clean records. */
    if (result->flags == INTEGRITY_OK) {
        strncpy(result->explanation, "OK",
                ALGAOPS_EXPLANATION_LEN - 1);
        result->explanation[ALGAOPS_EXPLANATION_LEN - 1] = '\0';
    }

    /* ------------------------------------------------------------------ */
    /* 7. Update state for next call                                       */
    /*    Advance sequence/timestamp tracking whenever records move strictly*/
    /*    forward, ensuring transport sequence tracking remains coherent   */
    /*    even if physical sensor readings (e.g. stuck sensor) fail, and   */
    /*    preventing duplicate missing packet warnings on subsequent steps.*/
    /* ------------------------------------------------------------------ */
    if (!state->has_previous) {
        state->has_previous      = true;
        state->prev_sequence_id  = record->sequence_id;
        state->prev_timestamp_ms = record->timestamp_ms;
    } else {
        if (record->sequence_id > state->prev_sequence_id) {
            state->prev_sequence_id = record->sequence_id;
        }
        if (record->timestamp_ms > state->prev_timestamp_ms) {
            state->prev_timestamp_ms = record->timestamp_ms;
        }
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

    struct { IntegrityFlags bit; const char *name; } map[] = {
        { INTEGRITY_MISSING_PACKET,    "MISSING_PACKET"    },
        { INTEGRITY_DUPLICATE_SEQ,     "DUPLICATE_SEQ"     },
        { INTEGRITY_OUT_OF_ORDER_SEQ,  "OUT_OF_ORDER_SEQ"  },
        { INTEGRITY_BAD_TIMESTAMP,     "BAD_TIMESTAMP"     },
        { INTEGRITY_VIB_OUT_OF_RANGE,  "VIB_OUT_OF_RANGE"  },
        { INTEGRITY_TEMP_OUT_OF_RANGE, "TEMP_OUT_OF_RANGE" },
        { INTEGRITY_VIB_STUCK,         "VIB_STUCK"         },
        { INTEGRITY_TEMP_STUCK,        "TEMP_STUCK"         },
        { INTEGRITY_NON_FINITE,        "NON_FINITE"        },
        { INTEGRITY_MALFORMED,         "MALFORMED"         },
    };

    bool first = true;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (flags & map[i].bit) {
            if (!first) strncat(buf, "|", (size_t)(buflen - (int)strlen(buf) - 1));
            strncat(buf, map[i].name, (size_t)(buflen - (int)strlen(buf) - 1));
            first = false;
        }
    }
}
