// Data integrity: packet status, completeness, sequence gaps, TrustGuard flags.

import type { PacketCounts } from "../api/types";
import { ApiError, api } from "../api/client";
import { store } from "../state/store";
import { packetStatusLegend } from "../components/packet_status_legend";
import { sequenceGapChart, type SeqItem } from "../components/sequence_gap_chart";
import { flagText, fmtPercent } from "../util/format";
import { fmtAgo } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";
import { nodeSelect } from "./trends";

interface Packets {
  counts: PacketCounts;
  completeness: number | null;
  gaps: { boot_id: number; from_seq: number; to_seq: number; missing: number; detected_ms: number }[];
  recent: SeqItem[];
}

export function mountIntegrity(root: HTMLElement): () => void {
  const body = h("div", {});
  root.append(h("h2", {}, "Data integrity (TrustGuard)"),
    h("div", { class: "toolbar" }, h("label", {}, "Node ", nodeSelect(() => void load()))), body);
  const load = async () => {
    const n = store.selectedNode();
    if (!n) {
      replaceChildren(body, stateBox("empty", "No node has reported yet."));
      return;
    }
    try {
      const p = await api.get<Packets>("/api/history/packets", { node_id: n.node_id,
                                                                 since_ms: Date.now() - 3600_000 });
      const flags = n.latest?.trust.flags ?? [];
      replaceChildren(body, h("div", { class: "grid two" },
        h("section", { class: "card" }, h("h3", {}, "Packet status (last hour)"), packetStatusLegend(p.counts),
          h("p", {}, `Completeness: ${fmtPercent(p.completeness, 1)} · liveness: ${n.liveness} · last seen ${fmtAgo(n.last_seen_ms)}`)),
        h("section", { class: "card" }, h("h3", {}, "Latest TrustGuard result"),
          n.latest ? h("div", {}, h("p", {}, `TrustScore ${n.latest.trust.score} — ${n.latest.trust.trusted ? "trusted" : "NOT trusted"}`),
            flags.length ? h("ul", {}, ...flags.map((f) => h("li", {}, h("code", {}, f), ` ${flagText(f)}`)))
              : h("p", { class: "muted" }, "no flags"),
            h("p", { class: "muted" }, n.latest.trust.explanation)) : stateBox("empty", "no decision yet"))),
        h("section", { class: "card" }, h("h3", {}, "Recent sequence numbers"),
          h("p", { class: "muted" }, "Each cell is one window; lost windows are drawn as gaps and are never treated as valid."),
          sequenceGapChart(p.recent)),
        h("section", { class: "card" }, h("h3", {}, "Detected gaps"), !p.gaps.length ? stateBox("empty", "No gaps in the last hour.")
          : h("table", { class: "data" }, h("thead", {}, h("tr", {}, h("th", {}, "Detected"), h("th", {}, "Boot"),
            h("th", {}, "Sequence range"), h("th", {}, "Missing"))),
            h("tbody", {}, ...p.gaps.map((g) => h("tr", {}, h("td", {}, fmtAgo(g.detected_ms)), h("td", {}, String(g.boot_id)),
              h("td", {}, `${g.from_seq}–${g.to_seq}`), h("td", {}, String(g.missing))))))));
    } catch (e) {
      replaceChildren(body, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };
  void load();
  const timer = setInterval(load, 10_000);
  return () => clearInterval(timer);
}
