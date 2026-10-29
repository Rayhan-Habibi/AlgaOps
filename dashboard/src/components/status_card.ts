// Per-node diagnostic status card.

import type { NodeSummary } from "../api/types";
import { OUTCOME_HINT, OUTCOME_LABEL, fmtPercent, fmtUnit, outcomeClass } from "../util/format";
import { fmtAgo } from "../util/time";
import { h } from "../util/dom";
import { trustBlock } from "./trust_score";

export function statusCard(n: NodeSummary, onOpen?: (n: NodeSummary) => void): HTMLElement {
  const d = n.latest;
  const outcome = d?.outcome ?? "NOT_EVALUATED";
  const f = d?.features ?? {};
  const live = n.liveness === "live" ? "" : ` · ${n.liveness}`;
  return h("article", { class: `card status ${outcomeClass(outcome)}`, "data-testid": "node-card",
                        "data-node": n.node_id },
    h("header", {},
      h("div", {}, h("h3", {}, n.node_id),
        h("span", { class: `src ${n.data_source}` },
          n.data_source === "hardware" ? "LIVE HARDWARE" : n.data_source.toUpperCase())),
      h("span", { class: "muted" }, `seen ${fmtAgo(n.last_seen_ms)}${live}`)),
    h("div", { class: "outcome", "data-testid": "outcome" }, OUTCOME_LABEL[outcome]),
    h("p", { class: "hint" }, OUTCOME_HINT[outcome]),
    d ? h("p", { class: "reason", title: d.reason }, d.reason) : h("p", { class: "muted" }, "no decision yet"),
    h("dl", { class: "kv" },
      h("dt", {}, "Mode"), h("dd", {}, d?.mode ?? "—"),
      h("dt", {}, "Vibration"), h("dd", {}, fmtUnit(f["vib_rms_g"], "g", 4)),
      h("dt", {}, "Temperature"), h("dd", {}, fmtUnit(f["temp_c"], "°C", 2)),
      h("dt", {}, "Current"), h("dd", {}, fmtUnit(f["current_a"], "A", 3)),
      h("dt", {}, "Completeness (10 min)"), h("dd", {}, fmtPercent(n.completeness_10min, 1)),
      h("dt", {}, "Connection"), h("dd", {}, n.connection ?? "—")),
    d ? trustBlock(d.trust.score, d.trust.trusted, d.trust.flags) : null,
    n.open_alert ? h("div", { class: `alert-strip ${outcomeClass(n.open_alert.kind)}` },
      `Open alert #${n.open_alert.id}: ${OUTCOME_LABEL[n.open_alert.kind]} since ${fmtAgo(n.open_alert.opened_ms)}`)
      : null,
    onOpen ? h("button", { class: "link", onclick: () => onOpen(n) }, "Trends & details →") : null);
}
