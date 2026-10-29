"""MQTT topic layout (docs/mqtt-contract.md).

    <prefix>/nodes/<node_id>/telemetry   telemetry_window.v1, QoS 1, not retained
    <prefix>/nodes/<node_id>/status      node_status.v1, QoS 1, retained (+ LWT)

There is deliberately no command topic: the system is read-only and never
publishes anything to sensor nodes, pumps, blowers or the PBR.
"""

from __future__ import annotations

from .payload_schema import NODE_ID_RE

DEFAULT_PREFIX = "algaops/v1"


def telemetry_topic(node_id: str, prefix: str = DEFAULT_PREFIX) -> str:
    return f"{prefix}/nodes/{node_id}/telemetry"


def status_topic(node_id: str, prefix: str = DEFAULT_PREFIX) -> str:
    return f"{prefix}/nodes/{node_id}/status"


def subscriptions(prefix: str = DEFAULT_PREFIX) -> list[str]:
    return [f"{prefix}/nodes/+/telemetry", f"{prefix}/nodes/+/status"]


def parse_topic(topic: str, prefix: str = DEFAULT_PREFIX) -> tuple[str, str] | None:
    """Return (kind, node_id) for a valid topic, else None."""
    head = prefix + "/nodes/"
    if not topic.startswith(head):
        return None
    rest = topic[len(head):].split("/")
    if len(rest) != 2 or rest[1] not in ("telemetry", "status"):
        return None
    if not NODE_ID_RE.match(rest[0]):
        return None
    return rest[1], rest[0]
