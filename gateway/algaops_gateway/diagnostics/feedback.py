"""Technician feedback on alerts (true alarm / false alarm / unsure).

Feedback is evidence for later threshold calibration and evaluation
datasets; it never changes a stored decision retroactively.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

VERDICTS = ("true_alarm", "false_alarm", "unsure")
_CTRL = re.compile(r"[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]")


class FeedbackError(ValueError):
    pass


@dataclass(frozen=True)
class Feedback:
    alert_id: int
    verdict: str
    note: str
    technician: str


def validate_feedback(alert_id: int, verdict: str, note: str | None, technician: str | None
                      ) -> Feedback:
    if not isinstance(alert_id, int) or isinstance(alert_id, bool) or alert_id < 1:
        raise FeedbackError("alert_id must be a positive integer")
    if verdict not in VERDICTS:
        raise FeedbackError(f"verdict must be one of {VERDICTS}")
    note = (note or "").strip()
    technician = (technician or "").strip()
    if len(note) > 500:
        raise FeedbackError("note must be at most 500 characters")
    if not technician or len(technician) > 64:
        raise FeedbackError("technician name/initials (1-64 characters) are required")
    if _CTRL.search(note) or _CTRL.search(technician):
        raise FeedbackError("control characters are not allowed")
    return Feedback(alert_id, verdict, note, technician)
