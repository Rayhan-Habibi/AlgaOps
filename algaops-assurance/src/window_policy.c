/*
 * window_policy.c — completeness ring and stream liveness.
 *
 * The ring stores one byte per expected sequence slot.  Every in-order
 * arrival pushes `missing` zero-slots followed by a one-slot; the arrived
 * counter is maintained incrementally so each update is O(missing) bounded
 * by the ring size.
 */

#include "../include/window_policy.h"

#include <string.h>

static uint32_t capacity(const WindowPolicyConfig *cfg)
{
    uint32_t c = cfg->completeness_window;
    if (c > WINDOW_POLICY_MAX_SLOTS) c = WINDOW_POLICY_MAX_SLOTS;
    return c;
}

void window_policy_init(WindowPolicyState *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

static void push_slot(WindowPolicyState *s, uint32_t cap, uint8_t v)
{
    if (s->span == cap) {
        /* overwrite oldest slot */
        if (s->slots[s->head]) s->arrived--;
    } else {
        s->span++;
    }
    s->slots[s->head] = v;
    if (v) s->arrived++;
    s->head = (s->head + 1u) % cap;
}

double window_policy_record(WindowPolicyState *s, const WindowPolicyConfig *cfg,
                            uint32_t missing)
{
    if (!s || !cfg) return -1.0;
    uint32_t cap = capacity(cfg);
    if (cap == 0) return -1.0;

    if (missing >= cap) {
        /* The whole ring is lost: reset to cap-1 missing slots. */
        memset(s->slots, 0, sizeof(s->slots));
        s->head = 0;
        s->span = 0;
        s->arrived = 0;
        missing = cap - 1u;
    }
    for (uint32_t i = 0; i < missing; i++) push_slot(s, cap, 0);
    push_slot(s, cap, 1);
    return window_policy_completeness(s, cfg);
}

double window_policy_completeness(const WindowPolicyState *s,
                                  const WindowPolicyConfig *cfg)
{
    if (!s || !cfg || capacity(cfg) == 0 || s->span == 0) return -1.0;
    return (double)s->arrived / (double)s->span;
}

bool window_policy_low_completeness(const WindowPolicyState *s,
                                    const WindowPolicyConfig *cfg)
{
    if (!s || !cfg || capacity(cfg) == 0) return false;
    uint32_t min_span = cfg->min_span;
    if (min_span > capacity(cfg)) min_span = capacity(cfg);
    if (s->span < min_span || s->span == 0) return false;
    return window_policy_completeness(s, cfg) < cfg->min_completeness;
}

void window_policy_touch(WindowPolicyState *s, uint64_t received_ms)
{
    if (!s) return;
    if (!s->has_data || received_ms >= s->last_received_ms) {
        s->last_received_ms = received_ms;
    }
    s->has_data = true;
    s->timeout_reported = false;
}

bool window_policy_tick(WindowPolicyState *s, const WindowPolicyConfig *cfg,
                        uint64_t now_ms, uint64_t *silence_ms)
{
    if (silence_ms) *silence_ms = 0;
    if (!s || !cfg || cfg->stream_timeout_ms == 0 || !s->has_data) return false;
    if (now_ms <= s->last_received_ms) return false;
    uint64_t silence = now_ms - s->last_received_ms;
    if (silence_ms) *silence_ms = silence;
    if (silence > cfg->stream_timeout_ms && !s->timeout_reported) {
        s->timeout_reported = true;
        return true;
    }
    return false;
}
