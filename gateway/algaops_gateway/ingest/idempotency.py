"""Idempotent ingestion.

MQTT QoS 1 is at-least-once: the broker may deliver the same PUBLISH again
(e.g. after a reconnect before the PUBACK).  An exact redelivery — same
(node_id, boot_id, seq) AND the same payload digest — is dropped here and
logged as `duplicate_delivery`; it never reaches the core, so transport
retries cannot create false DUPLICATE_SEQ alarms.  A *different* payload
reusing a sequence number is not a redelivery: it is passed to the core,
which reports DUPLICATE_SEQ -> CHECK_SENSOR.  The database UNIQUE constraint
on (node_id, boot_id, seq, payload_digest) is the persistent backstop.
"""

from __future__ import annotations

import sqlite3
from collections import OrderedDict


class IdempotencyGuard:
    def __init__(self, capacity: int = 20000) -> None:
        self._seen: OrderedDict[tuple[str, int, int, str], None] = OrderedDict()
        self._capacity = capacity
        self.duplicates = 0

    def warm(self, conn: sqlite3.Connection, per_node: int = 2000) -> None:
        nodes = [r[0] for r in conn.execute("SELECT node_id FROM nodes")]
        for node in nodes:
            rows = conn.execute(
                "SELECT boot_id, seq, payload_digest FROM windows WHERE node_id = ? "
                "ORDER BY id DESC LIMIT ?", (node, per_node)).fetchall()
            for r in reversed(rows):
                self.remember(node, int(r[0]), int(r[1]), r[2])

    def is_duplicate(self, node_id: str, boot_id: int, seq: int, digest: str) -> bool:
        key = (node_id, boot_id, seq, digest)
        if key in self._seen:
            self._seen.move_to_end(key)
            self.duplicates += 1
            return True
        return False

    def remember(self, node_id: str, boot_id: int, seq: int, digest: str) -> None:
        key = (node_id, boot_id, seq, digest)
        self._seen[key] = None
        self._seen.move_to_end(key)
        while len(self._seen) > self._capacity:
            self._seen.popitem(last=False)
