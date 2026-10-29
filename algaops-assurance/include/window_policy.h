/**
 * @file window_policy.h
 * @brief Stream-level diagnostic-window policy: completeness and liveness.
 *
 * A single missing packet is informational (TrustGuard MISSING_PACKET), but a
 * diagnostic decision that relies on recent history (stuck detection, mode
 * inference, baselines) needs a sufficiently complete stream.  This module
 * keeps a ring of the most recent expected sequence slots and reports
 * INTEGRITY_LOW_COMPLETENESS when too many of them never arrived.  It also
 * detects stream silence (no window within stream_timeout_ms), which the
 * engine turns into a CHECK_SENSOR decision so that a stale NORMAL is never
 * left on display.
 */

#ifndef WINDOW_POLICY_H
#define WINDOW_POLICY_H

#include "algaops_types.h"

/** Maximum completeness window (sequence slots). */
#define WINDOW_POLICY_MAX_SLOTS 1024

typedef struct {
    /** Number of recent expected sequence slots considered. 0 disables. */
    uint32_t completeness_window;
    /** Minimum fraction of slots that must have arrived, e.g. 0.8. */
    double   min_completeness;
    /** Completeness is only judged once this many slots have been observed. */
    uint32_t min_span;
    /** No telemetry for this long => stream timeout (tick). 0 disables. */
    uint32_t stream_timeout_ms;
} WindowPolicyConfig;

static inline WindowPolicyConfig window_policy_default_config(void)
{
    WindowPolicyConfig c;
    c.completeness_window = 0;      /* disabled: v1 behaviour */
    c.min_completeness    = 0.8;
    c.min_span            = 30;
    c.stream_timeout_ms   = 0;      /* disabled */
    return c;
}

typedef struct {
    uint8_t  slots[WINDOW_POLICY_MAX_SLOTS]; /* 1 = arrived, 0 = missing */
    uint32_t head;          /* next write index                          */
    uint32_t span;          /* slots filled so far (<= completeness_window) */
    uint32_t arrived;       /* number of 1-slots within span             */
    bool     has_data;
    uint64_t last_received_ms;
    bool     timeout_reported;
} WindowPolicyState;

void window_policy_init(WindowPolicyState *s);

/**
 * @brief Record one in-order arrival preceded by @p missing lost windows.
 * @return Current completeness in [0,1], or -1.0 when disabled.
 */
double window_policy_record(WindowPolicyState *s, const WindowPolicyConfig *cfg,
                            uint32_t missing);

/** Current completeness in [0,1], or -1.0 when disabled / no data. */
double window_policy_completeness(const WindowPolicyState *s,
                                  const WindowPolicyConfig *cfg);

/** True when enough slots are observed and completeness is below minimum. */
bool window_policy_low_completeness(const WindowPolicyState *s,
                                    const WindowPolicyConfig *cfg);

/** Note that a window (any window) was received at @p received_ms. */
void window_policy_touch(WindowPolicyState *s, uint64_t received_ms);

/**
 * @brief Liveness check.  Returns true exactly once per silence period when
 *        now_ms - last_received_ms exceeds stream_timeout_ms.
 * @param silence_ms  optional output: length of the silence so far.
 */
bool window_policy_tick(WindowPolicyState *s, const WindowPolicyConfig *cfg,
                        uint64_t now_ms, uint64_t *silence_ms);

#endif /* WINDOW_POLICY_H */
