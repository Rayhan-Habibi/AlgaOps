// Overview: gateway health, per-node status cards, open alerts, live events.

import type { Alert } from "../api/types";
import { api } from "../api/client";
import { store } from "../state/store";
import { statusCard } from "../components/status_card";
import { eventLog } from "../components/event_log";
import { OUTCOME_LABEL, outcomeClass, shortHash } from "../util/format";
import { fmtAgo, fmtDuration } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";

export function mountOverview(root: HTMLElement): () => void {
  const gw = h("section", { class: "strip", "data-testid": "gateway-strip" });
  const cards = h("section", { class: "grid cards" });
  const alerts = h("section", { class: "card" });
  const log = h("section", { class: "card" });
  root.append(h("h2", {}, "Overview"), gw, cards,
              h("div", { class: "grid two" }, alerts, log));

  let alertsData: Alert[] | null = null;
  let alertsErr: string | null = null;
  const loadAlerts = async () => {
    try {
      alertsData = (await api.get<{ items: Alert[] }>("/api/alerts", { status: "open" })).items;
      alertsErr = null;
    } catch (e) {
      alertsErr = (e as Error).message;
    }
    render();
  };

  const render = () => {
    const s = store.get();
    const st = s.status;
    replaceChildren(gw, st
      ? h("dl", { class: "kv inline" },
          h("dt", {}, "Gateway"), h("dd", {}, `${st.gateway_id} · ${st.site}`),
          h("dt", {}, "Uptime"), h("dd", {}, fmtDuration((st.uptime_s ?? 0) * 1000)),
          h("dt", {}, "Policy"), h("dd", { title: st.policy.hash },
            `${st.policy.name ?? "?"} ${st.policy.version ?? ""} (${shortHash(st.policy.hash, 8)})`),
          h("dt", {}, "Windows accepted"), h("dd", {}, String(st.pipeline.accepted)),
          h("dt", {}, "Rejected / redelivered"),
          h("dd", {}, `${st.pipeline.rejected} / ${st.pipeline.duplicate_deliveries}`),
          h("dt", {}, "Decision latency p95"),
          h("dd", {}, st.pipeline.latency_ms.p95 !== null ? `${st.pipeline.latency_ms.p95} ms` : "—"),
          h("dt", {}, "Audit head"), h("dd", {}, `#${st.audit.head_seq}`),
          h("dt", {}, "Last verification"),
          h("dd", {}, st.audit.last_verification
            ? `${st.audit.last_verification.ok ? "OK" : "FAILED"}` : "not run yet"),
          h("dt", {}, "Mode"), h("dd", {}, "read-only · local network · no plant control"))
      : (s.apiError ? stateBox("error", `Gateway unreachable: ${s.apiError}`)
        : stateBox("loading", "connecting to gateway…")));

    if (!s.apiOk && !s.nodes.length) {
      replaceChildren(cards, s.apiError ? stateBox("error", "No data: the gateway API is not reachable.")
        : stateBox("loading", "loading nodes…"));
    } else if (!s.nodes.length) {
      replaceChildren(cards, stateBox("empty", "No sensor node has reported yet. Start a node or a "
        + "simulator scenario (Replay page)."));
    } else {
      replaceChildren(cards, ...s.nodes.map((n) => statusCard(n, (node) => {
        store.set({ selectedNode: node.node_id });
        location.hash = "#/trends";
      })));
    }

    replaceChildren(alerts, h("h3", {}, "Open alerts"),
      alertsErr ? stateBox("error", alertsErr)
        : alertsData === null ? stateBox("loading", "loading…")
          : !alertsData.length ? stateBox("empty", "No open alerts.")
            : h("ul", { class: "alert-list" }, ...alertsData.map((a) =>
              h("li", { class: outcomeClass(a.kind) },
                h("a", { href: `#/events?alert=${a.id}` }, `#${a.id} ${a.node_id}`),
                ` ${OUTCOME_LABEL[a.kind]} · ${fmtAgo(a.opened_ms)} · ${a.last_reason.slice(0, 140)}`))));
    replaceChildren(log, h("h3", {}, "Live events"), eventLog(s.liveEvents));
  };

  const unsub = store.subscribe((s) => {
    render();
    if (s.liveEvents[0]?.type === "alert") void loadAlerts();
  });
  render();
  void loadAlerts();
  const timer = setInterval(loadAlerts, 15_000);
  return () => {
    unsub();
    clearInterval(timer);
  };
}
