"""node_status.v1 messages (retained status + MQTT last-will "offline").

Status messages only inform the connection indicator on the dashboard; they
never change a diagnostic decision.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any

from .payload_schema import NODE_ID_RE, SOURCES, PayloadError

SCHEMA_ID = "algaops.node_status.v1"
STATES = ("online", "offline")
ALLOWED = {"schema", "node_id", "state", "boot_id", "fw", "uptime_s", "rssi_dbm", "queue_depth",
           "dropped_windows", "self_test", "sensors", "source", "reset_reason", "ts_ms"}


@dataclass(frozen=True)
class NodeStatus:
    node_id: str
    state: str
    received_ms: int
    fields: dict[str, Any]

    def to_record(self) -> dict[str, Any]:
        return {"node_id": self.node_id, "state": self.state, "received_ms": self.received_ms,
                **{k: v for k, v in sorted(self.fields.items())}}


def parse_status(raw: bytes, received_ms: int, topic_node_id: str) -> NodeStatus:
    if len(raw) > 4096:
        raise PayloadError("status payload too large", topic_node_id)
    try:
        doc = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        raise PayloadError("status payload is not valid JSON", topic_node_id) from None
    if not isinstance(doc, dict):
        raise PayloadError("status payload must be an object", topic_node_id)
    if doc.get("schema") != SCHEMA_ID:
        raise PayloadError("unsupported status schema", topic_node_id)
    if set(doc) - ALLOWED:
        raise PayloadError("unexpected status field(s)", topic_node_id)
    nid = doc.get("node_id")
    if not isinstance(nid, str) or not NODE_ID_RE.match(nid) or nid != topic_node_id:
        raise PayloadError("status node_id invalid or does not match topic", topic_node_id)
    if doc.get("state") not in STATES:
        raise PayloadError("status state must be online/offline", topic_node_id)
    if "source" in doc and doc["source"] not in SOURCES:
        raise PayloadError("status source invalid", topic_node_id)
    for k in ("boot_id", "uptime_s", "queue_depth", "dropped_windows", "rssi_dbm", "ts_ms"):
        if k in doc and (not isinstance(doc[k], int) or isinstance(doc[k], bool)):
            raise PayloadError(f"status {k} must be an integer", topic_node_id)
    for k in ("fw", "self_test", "reset_reason"):
        if k in doc and (not isinstance(doc[k], str) or len(doc[k]) > 64):
            raise PayloadError(f"status {k} must be a short string", topic_node_id)
    if "sensors" in doc and (not isinstance(doc["sensors"], dict) or
                             not all(isinstance(v, bool) for v in doc["sensors"].values())):
        raise PayloadError("status sensors must map names to booleans", topic_node_id)
    fields = {k: v for k, v in doc.items() if k not in ("schema", "node_id", "state")}
    return NodeStatus(nid, doc["state"], int(received_ms), fields)
