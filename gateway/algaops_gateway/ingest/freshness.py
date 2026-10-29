"""Node liveness classification for the dashboard.

This is a display aid only.  The diagnostic consequence of silence (a
CHECK_SENSOR decision with STREAM_TIMEOUT) and of late windows (STALE) is
decided by the C core (window policy / TrustGuard), never here.
"""

from __future__ import annotations

LIVE, LATE, STALE, NEVER = "live", "late", "stale", "never"


def classify_liveness(now_ms: int, last_received_ms: int | None, window_ms: int | None,
                      stale_after_ms: int) -> str:
    if last_received_ms is None:
        return NEVER
    age = max(0, now_ms - last_received_ms)
    period = window_ms or 1000
    if age <= 3 * period:
        return LIVE
    if age <= stale_after_ms:
        return LATE
    return STALE


def completeness(received: int, expected: int) -> float | None:
    """Fraction of expected windows that arrived (None when nothing expected)."""
    if expected <= 0:
        return None
    return max(0.0, min(1.0, received / expected))
