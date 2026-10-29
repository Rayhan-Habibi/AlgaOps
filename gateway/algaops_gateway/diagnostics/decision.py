"""Conversion of core decisions into diagnostic_event.v1 records.

No decision logic lives here: every field is copied from the AlgaDecision
returned by the C core.  The canonical JSON of the record is stored and its
Ascon-Hash256 digest is bound into the audit chain.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from ..audit.canonical import canonical_json, finite_or_none
from ..core.libalgaops import (
    ALERT_EVENTS,
    DECISION_KINDS,
    FEATURES,
    METHOD_STATUS,
    METHODS,
    MODES,
    OUTCOMES,
    AlgaDecision,
    CoreLibrary,
)

SCHEMA_ID = "algaops.diagnostic_event.v1"


def _outcome(i: int) -> str | None:
    return OUTCOMES[i] if 0 <= i < len(OUTCOMES) else None


@dataclass
class DecisionRecord:
    doc: dict[str, Any]

    @property
    def outcome(self) -> str:
        return self.doc["outcome"]

    @property
    def is_trusted(self) -> bool:
        return self.doc["trust"]["trusted"]

    @property
    def mode(self) -> str:
        return self.doc["mode"]

    @property
    def kind(self) -> str:
        return self.doc["kind"]

    @property
    def alert_event(self) -> str:
        return self.doc["alert"]["event"]

    def canonical(self) -> str:
        return canonical_json(self.doc)

    @classmethod
    def from_core(cls, lib: CoreLibrary, d: AlgaDecision, *, node_id: str, data_source: str,
                  decided_ms: int, policy_hash: str, window_digest: str | None) -> DecisionRecord:
        features = {FEATURES[i]: finite_or_none(d.features[i])
                    for i in range(len(FEATURES)) if d.feature_mask & (1 << i)}
        methods: dict[str, Any] = {}
        for m, name in enumerate(METHODS):
            r = d.methods[m]
            status = METHOD_STATUS[r.status] if 0 <= r.status < len(METHOD_STATUS) else "unknown"
            entry: dict[str, Any] = {"status": status}
            if status == "ok":
                entry.update({
                    "anomalous": bool(r.anomalous),
                    "score": finite_or_none(r.score),
                    "threshold": finite_or_none(r.threshold),
                    "dims": int(r.dims),
                    "top_feature": FEATURES[r.top_feature] if 0 <= r.top_feature < 4 else None,
                    "feature_scores": {FEATURES[i]: finite_or_none(r.feature_z[i])
                                       for i in range(len(FEATURES)) if r.feature_z[i] != 0.0},
                })
            methods[name] = entry
        primary = METHODS[d.primary_method] if 0 <= d.primary_method < len(METHODS) else None
        decided_by = METHODS[d.decided_by] if 0 <= d.decided_by < len(METHODS) else None
        doc = {
            "schema": SCHEMA_ID,
            "kind": DECISION_KINDS[d.kind] if 0 <= d.kind < len(DECISION_KINDS) else "window",
            "node_id": node_id,
            "data_source": data_source,
            "boot_id": int(d.boot_id),
            "seq": int(d.sequence_id),
            "timestamp_ms": int(d.timestamp_ms),
            "received_ms": int(d.received_ms),
            "decided_ms": int(decided_ms),
            "trust": {
                "score": int(d.trust_score),
                "trusted": bool(d.is_trusted),
                "flag_bits": int(d.flags),
                "flags": lib.flag_list(int(d.flags)),
                "blocking": bool(d.flags & lib.blocking_flags),
                "explanation": d.explanation.decode("utf-8", errors="replace"),
            },
            "outcome": _outcome(d.outcome) or "UNKNOWN",
            "mode": MODES[d.mode] if 0 <= d.mode < len(MODES) else "UNKNOWN",
            "mode_changed": bool(d.mode_changed),
            "time_in_mode_ms": int(d.time_in_mode_ms),
            "method": primary,
            "decided_by": decided_by,
            "baseline_id": int(d.baseline_id) or None,
            "features": features,
            "methods": methods,
            "alert": {
                "event": ALERT_EVENTS[d.alert_event] if 0 <= d.alert_event < 4 else "NONE",
                "kind": _outcome(d.alert_kind),
                "prev_kind": _outcome(d.alert_prev_kind),
                "consecutive": int(d.consecutive),
            },
            "stream": {
                "missing_count": int(d.missing_count),
                "completeness": finite_or_none(d.completeness) if d.completeness >= 0 else None,
            },
            "reason": d.reason.decode("utf-8", errors="replace"),
            "policy_hash": policy_hash,
            "engine_version": lib.version,
            "window_digest": window_digest,
        }
        return cls(doc)
