// Alerts and decision history with full traceability (flags, reason, methods,
// window record, audit event) and technician feedback.

import type { Alert, Decision } from "../api/types";
import { ApiError, api } from "../api/client";
import { feedbackButtons } from "../components/feedback_buttons";
import { trustBlock } from "../components/trust_score";
import { OUTCOME_LABEL, fmtNumber, flagText, outcomeClass, shortHash } from "../util/format";
import { fmtDateTime, fmtTime } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";

interface DiagItem {
  id: number; node_id: string; kind: string; outcome: string; trust_score: number; trusted: boolean;
  flags: string[]; mode: string; method: string | null; seq: number; decided_ms: number;
  data_source: string; reason: string; alert_event: string;
}

interface DiagDetail {
  id: number; decision: Decision; decision_digest: string; latency_ms: number | null;
  window: { id: number; record: Record<string, unknown>; record_digest: string; payload_digest: string } | null;
  audit_event: { seq: number; type: string; hash: string; prev_hash: string } | null;
}

function decisionDetail(d: DiagDetail): HTMLElement {
  const dec = d.decision;
  const methods = Object.entries(dec.methods);
  return h("div", { class: "detail", "data-testid": "decision-detail" },
    h("h4", { class: outcomeClass(dec.outcome) }, `Decision #${d.id}: ${OUTCOME_LABEL[dec.outcome]}`),
    h("p", {}, dec.reason),
    trustBlock(dec.trust.score, dec.trust.trusted, dec.trust.flags),
    dec.trust.flags.length ? h("ul", { class: "flags" }, ...dec.trust.flags.map((f) =>
      h("li", {}, h("code", {}, f), ` — ${flagText(f)}`))) : null,
    h("p", { class: "muted" }, `TrustGuard: ${dec.trust.explanation}`),
    h("dl", { class: "kv" },
      h("dt", {}, "Node / boot / seq"), h("dd", {}, `${dec.node_id} / ${dec.boot_id} / ${dec.seq}`),
      h("dt", {}, "Window end (node clock)"), h("dd", {}, fmtDateTime(dec.timestamp_ms)),
      h("dt", {}, "Received / decided"),
      h("dd", {}, `${fmtTime(dec.received_ms)} / ${fmtTime(dec.decided_ms)}`),
      h("dt", {}, "Mode / method"), h("dd", {}, `${dec.mode} / ${dec.method ?? "—"} (decided by ${dec.decided_by ?? "—"})`),
      h("dt", {}, "Data source"), h("dd", {}, dec.data_source),
      h("dt", {}, "Policy / engine"), h("dd", {}, `${shortHash(dec.policy_hash, 12)} / ${dec.engine_version}`),
      h("dt", {}, "Decision digest"), h("dd", {}, h("code", {}, shortHash(d.decision_digest, 24))),
      h("dt", {}, "Audit event"),
      h("dd", {}, d.audit_event ? h("code", {}, `#${d.audit_event.seq} ${shortHash(d.audit_event.hash, 20)}`) : "—")),
    methods.length ? h("table", { class: "data" },
      h("thead", {}, h("tr", {}, h("th", {}, "Method"), h("th", {}, "Status"), h("th", {}, "Score"),
        h("th", {}, "Threshold"), h("th", {}, "Flags deviation"))),
      h("tbody", {}, ...methods.map(([m, r]) => h("tr", {}, h("td", {}, m + (m === dec.method ? " (primary)" : "")),
        h("td", {}, r.status), h("td", {}, fmtNumber(r.score ?? null, 2)),
        h("td", {}, fmtNumber(r.threshold ?? null, 2)),
        h("td", {}, r.status === "ok" ? (r.anomalous ? "yes" : "no") : "—"))))) : null,
    d.window ? h("details", {}, h("summary", {}, "Window record (as stored and hashed)"),
      h("pre", {}, JSON.stringify(d.window.record, null, 2))) : null);
}

export function mountEvents(root: HTMLElement): () => void {
  const alertsBox = h("section", { class: "card" });
  const alertDetail = h("section", { class: "card" });
  const diagBox = h("section", { class: "card" });
  const detailBox = h("section", { class: "card", id: "decision-detail" });
  let outcome = "";
  const filter = h("select", { "aria-label": "outcome filter" },
    ...["", "CHECK_SENSOR", "CHECK_EQUIPMENT", "NOT_EVALUATED", "NORMAL"].map((o) =>
      h("option", { value: o }, o ? OUTCOME_LABEL[o as keyof typeof OUTCOME_LABEL] : "all outcomes")));
  filter.addEventListener("change", () => { outcome = filter.value; void loadDiag(); });
  root.append(h("h2", {}, "Alerts & decisions"), h("div", { class: "grid two" }, alertsBox, alertDetail),
    h("div", { class: "toolbar" }, h("label", {}, "Decision history ", filter)), diagBox, detailBox);

  const showDecision = async (id: number) => {
    replaceChildren(detailBox, stateBox("loading", "loading decision…"));
    try {
      replaceChildren(detailBox, decisionDetail(await api.get<DiagDetail>(`/api/diagnostics/${id}`)));
      detailBox.scrollIntoView({ behavior: "smooth", block: "start" });
    } catch (e) {
      replaceChildren(detailBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  const showAlert = async (id: number) => {
    replaceChildren(alertDetail, stateBox("loading", "loading alert…"));
    try {
      const r = await api.get<{ alert: Alert; decisions: Decision[]; feedback: { verdict: string; technician: string;
        note: string; created_ms: number }[] }>(`/api/alerts/${id}`);
      const a = r.alert;
      replaceChildren(alertDetail, h("h3", {}, `Alert #${a.id} · ${a.node_id}`),
        h("p", { class: outcomeClass(a.kind) }, `${OUTCOME_LABEL[a.kind]} · ${a.status} · opened ${fmtDateTime(a.opened_ms)}`),
        h("p", {}, a.last_reason),
        h("h4", {}, "Technician feedback"),
        r.feedback.length ? h("ul", {}, ...r.feedback.map((f) =>
          h("li", {}, `${f.verdict} by ${f.technician} (${fmtDateTime(f.created_ms)}) ${f.note}`)))
          : h("p", { class: "muted" }, "no feedback yet"),
        feedbackButtons(a.id, () => void showAlert(a.id)),
        h("h4", {}, `Decisions (${r.decisions.length})`),
        h("ul", { class: "mini" }, ...r.decisions.slice(-20).map((d) =>
          h("li", {}, h("a", { href: "#", onclick: (ev: Event) => { ev.preventDefault(); void showDecision(d.id ?? 0); } },
            `#${d.id} seq ${d.seq}`), ` ${d.trust.flags.join(", ") || d.reason.slice(0, 80)}`))));
    } catch (e) {
      replaceChildren(alertDetail, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  const loadAlerts = async () => {
    try {
      const items = (await api.get<{ items: Alert[] }>("/api/alerts", { limit: 100 })).items;
      replaceChildren(alertsBox, h("h3", {}, "Alerts"), !items.length ? stateBox("empty", "No alerts recorded.")
        : h("table", { class: "data", "data-testid": "alerts-table" },
          h("thead", {}, h("tr", {}, h("th", {}, "#"), h("th", {}, "Node"), h("th", {}, "Kind"),
            h("th", {}, "Status"), h("th", {}, "Opened"), h("th", {}, "Feedback"))),
          h("tbody", {}, ...items.map((a) => h("tr", { class: outcomeClass(a.kind) },
            h("td", {}, h("a", { href: "#", onclick: (ev: Event) => { ev.preventDefault(); void showAlert(a.id); } },
              String(a.id))),
            h("td", {}, a.node_id), h("td", {}, OUTCOME_LABEL[a.kind]), h("td", {}, a.status),
            h("td", {}, fmtDateTime(a.opened_ms)), h("td", {}, a.last_verdict ?? "—"))))));
    } catch (e) {
      replaceChildren(alertsBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  const loadDiag = async () => {
    try {
      const items = (await api.get<{ items: DiagItem[] }>("/api/diagnostics", { limit: 200, outcome })).items;
      replaceChildren(diagBox, !items.length ? stateBox("empty", "No decisions.")
        : h("table", { class: "data", "data-testid": "diag-table" },
          h("thead", {}, h("tr", {}, h("th", {}, "#"), h("th", {}, "Time"), h("th", {}, "Node"),
            h("th", {}, "Seq"), h("th", {}, "Outcome"), h("th", {}, "Trust"), h("th", {}, "Mode"),
            h("th", {}, "Flags"), h("th", {}, "Reason"))),
          h("tbody", {}, ...items.map((d) => h("tr", { class: outcomeClass(d.outcome) },
            h("td", {}, h("a", { href: "#", onclick: (ev: Event) => { ev.preventDefault(); void showDecision(d.id); } },
              String(d.id))),
            h("td", {}, fmtTime(d.decided_ms)), h("td", {}, d.node_id), h("td", {}, String(d.seq)),
            h("td", {}, OUTCOME_LABEL[d.outcome as keyof typeof OUTCOME_LABEL] ?? d.outcome),
            h("td", {}, String(d.trust_score)), h("td", {}, d.mode), h("td", {}, d.flags.join(" ")),
            h("td", { class: "reason", title: d.reason }, d.reason))))));
    } catch (e) {
      replaceChildren(diagBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  replaceChildren(alertDetail, h("h3", {}, "Alert detail"), h("p", { class: "muted" }, "Select an alert."));
  void loadAlerts();
  void loadDiag();
  const m = /alert=(\d+)/.exec(location.hash);
  if (m) void showAlert(Number(m[1]));
  const timer = setInterval(() => { void loadAlerts(); void loadDiag(); }, 15_000);
  return () => clearInterval(timer);
}
