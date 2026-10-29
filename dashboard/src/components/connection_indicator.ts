// Gateway / live-stream / MQTT broker connection pills.

import type { AppState } from "../state/store";
import { fmtAgo } from "../util/time";
import { h } from "../util/dom";

function pill(label: string, state: "ok" | "warn" | "bad", title: string): HTMLElement {
  return h("span", { class: `pill ${state}`, title }, h("span", { class: "dot" }), label);
}

export function connectionIndicator(s: AppState): HTMLElement {
  const api = s.apiOk
    ? pill("Gateway", "ok", `API reachable, refreshed ${fmtAgo(s.lastRefreshMs)}`)
    : pill("Gateway", "bad", s.apiError ?? "gateway unreachable");
  const live = s.stream === "open" ? pill("Live", "ok", "event stream connected")
    : pill("Live", s.stream === "connecting" ? "warn" : "bad", `event stream ${s.stream}`);
  const mq = s.status?.mqtt;
  const broker = !mq ? pill("MQTT", "warn", "MQTT ingest disabled")
    : mq.state === "connected" ? pill("MQTT", "ok", `broker ${mq.host}:${mq.port}`)
      : pill("MQTT", "bad", `broker ${mq.state}${mq.last_error ? ` (${mq.last_error})` : ""}`);
  return h("div", { class: "conn", "data-testid": "connection" }, api, live, broker);
}
