// Live event log fed by the SSE stream.

import { OUTCOME_LABEL, outcomeClass } from "../util/format";
import { fmtTime } from "../util/time";
import { h } from "../util/dom";
import type { Outcome } from "../api/types";

export interface LogEntry { type: string; data: Record<string, unknown>; at: number }

function line(e: LogEntry): HTMLElement | null {
  const d = e.data;
  const node = String(d["node_id"] ?? "—");
  if (e.type === "decision") {
    const o = d["outcome"] as Outcome;
    if (o === "NORMAL" && !d["alert_event"] || o === "NORMAL" && d["alert_event"] === "NONE") return null;
    return h("li", { class: outcomeClass(o) }, h("time", {}, fmtTime(e.at)), h("b", {}, node),
             ` ${OUTCOME_LABEL[o] ?? o} — ${String(d["reason"] ?? "")}`);
  }
  if (e.type === "alert") {
    return h("li", { class: "alert" }, h("time", {}, fmtTime(e.at)), h("b", {}, node),
             ` alert ${String(d["event"] ?? "")} ${String(d["kind"] ?? d["verdict"] ?? "")}`);
  }
  if (e.type === "node") {
    return h("li", { class: "info" }, h("time", {}, fmtTime(e.at)), h("b", {}, node),
             ` node ${String(d["state"] ?? "")}`);
  }
  if (e.type === "packet") {
    return h("li", { class: "info" }, h("time", {}, fmtTime(e.at)), h("b", {}, node),
             ` packet ${String(d["status"] ?? "")}${d["reason"] ? `: ${String(d["reason"])}` : ""}`);
  }
  if (e.type === "baseline") {
    return h("li", { class: "info" }, h("time", {}, fmtTime(e.at)), h("b", {}, node),
             ` baseline ${String(d["status"] ?? d["error"] ?? "update")}`);
  }
  return null;
}

export function eventLog(entries: LogEntry[], max = 60): HTMLElement {
  const items = entries.map(line).filter((x): x is HTMLElement => x !== null).slice(0, max);
  if (!items.length) return h("p", { class: "muted" }, "No warnings or events since this page was opened.");
  return h("ul", { class: "event-log", "data-testid": "event-log" }, ...items);
}
