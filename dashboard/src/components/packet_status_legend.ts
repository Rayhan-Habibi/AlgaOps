// Packet status classes (matches the TrustGuard packet-status panel of the design).

import type { PacketCounts } from "../api/types";
import { h } from "../util/dom";

const ROWS: { key: keyof PacketCounts; label: string; cls: string; hint: string }[] = [
  { key: "valid", label: "valid", cls: "valid", hint: "trusted windows" },
  { key: "late_stale", label: "late / stale", cls: "late", hint: "delivered after the staleness limit" },
  { key: "duplicate", label: "duplicate", cls: "dup", hint: "same sequence id, different content" },
  { key: "out_of_order", label: "out of order", cls: "ooo", hint: "older sequence id or superseded session" },
  { key: "invalid", label: "invalid content", cls: "invalid", hint: "range, stuck, self-test, fault flags…" },
  { key: "lost", label: "lost · logged, never valid", cls: "lost", hint: "sequence gaps" },
  { key: "malformed", label: "malformed", cls: "invalid", hint: "rejected before decoding" },
  { key: "duplicate_delivery", label: "MQTT redelivery (dropped)", cls: "muted", hint: "exact retries, idempotent" },
];

export function packetStatusLegend(c: PacketCounts | null | undefined): HTMLElement {
  return h("ul", { class: "packet-legend", "data-testid": "packet-legend" }, ...ROWS.map((r) =>
    h("li", { title: r.hint }, h("span", { class: `sw ${r.cls}` }), h("span", {}, r.label),
      h("strong", {}, c ? String(c[r.key] ?? 0) : "—"))));
}
