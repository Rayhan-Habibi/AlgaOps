// Scenario replay (DEMO / SYNTHETIC): offline evaluation in an isolated
// database, or real-time publishing to the local broker.

import type { ReplayJob, ScenarioInfo, ScenarioResult } from "../api/types";
import { ApiError, api } from "../api/client";
import { OUTCOME_LABEL, fmtPercent, outcomeClass } from "../util/format";
import { h, replaceChildren, s, stateBox } from "../util/dom";

function timelineStrip(r: ScenarioResult): HTMLElement {
  const tl = r.timeline ?? [];
  const n = tl.length;
  const w = 900;
  const cell = Math.max(1, w / Math.max(1, n));
  const svg = s("svg", { viewBox: `0 0 ${w} 46`, class: "timeline", role: "img",
                         "aria-label": "ground truth (top) vs outcome (bottom)" });
  tl.forEach((t, i) => {
    const truth = s("rect", { x: i * cell, y: 2, width: cell, height: 18, class: `tr-${t.truth}` });
    truth.append(s("title", {}, `#${i} truth ${t.truth}${t.fault ? ` (${t.fault})` : ""}`));
    const out = s("rect", { x: i * cell, y: 26, width: cell, height: 18, class: `oc-${outcomeClass(t.outcome)}` });
    out.append(s("title", {}, `#${i} ${t.outcome}${t.flags?.length ? ` ${t.flags.join(",")}` : ""}`));
    svg.append(truth, out);
  });
  return h("div", { class: "tlwrap" }, h("div", { class: "muted small" }, "top: ground truth (kept outside payloads) · bottom: system outcome"), svg);
}

function resultView(r: ScenarioResult): HTMLElement {
  return h("div", { "data-testid": "replay-result" },
    h("h4", {}, `${r.name}: ${r.messages} messages, ${r.duration_s} s of data`),
    h("p", {}, r.description),
    timelineStrip(r),
    h("dl", { class: "kv inline" },
      ...Object.entries(r.outcome_counts).flatMap(([k, v]) => [h("dt", { class: outcomeClass(k) },
        OUTCOME_LABEL[k as keyof typeof OUTCOME_LABEL] ?? k), h("dd", {}, String(v))])),
    h("p", {}, `Matches expectation: ${fmtPercent(r.expected_match_rate, 1)} · CHECK_EQUIPMENT on faulty data: `
      + `${r.attribution_errors} · stream timeouts: ${r.stream_timeouts}`),
    r.fault_events.length ? h("ul", {}, ...r.fault_events.map((e) => h("li", {},
      `${e.fault} (${e.truth}): ${e.detected ? `detected after ${e.latency_s} s (${e.latency_messages} messages)` : "NOT detected"}`)))
      : h("p", { class: "muted" }, "no injected faults"));
}

export function mountReplay(root: HTMLElement): () => void {
  const listBox = h("section", { class: "card" });
  const jobBox = h("section", { class: "card job", "data-testid": "replay-job", "aria-live": "polite" });
  root.append(h("h2", {}, "Scenario replay"),
    h("p", { class: "disclaimer" }, "All scenarios are DEMO / SYNTHETIC. Offline runs use an isolated database; "
      + "live runs publish in real time and appear as simulator nodes on the overview."),
    h("div", { class: "grid two" }, listBox, jobBox));
  let poll: ReturnType<typeof setTimeout> | null = null;
  let alive = true;

  const watch = async (id: number) => {
    try {
      const j = await api.get<ReplayJob>(`/api/replay/jobs/${id}`);
      if (!alive) return;
      replaceChildren(jobBox, h("h3", {}, `Job #${j.id} · ${j.kind} · ${j.scenario}`),
        h("p", {}, `state: ${j.state}${j.error ? ` — ${j.error}` : ""}`),
        j.state === "running" ? h("progress", { max: 1, value: j.progress }) : null,
        j.kind === "live" && j.state === "running" ? h("button", { class: "btn", onclick: async () => {
          await api.post(`/api/replay/jobs/${j.id}/stop`); } }, "Stop") : null,
        j.result ? resultView(j.result) : null);
      if (j.state === "running") poll = setTimeout(() => void watch(id), 1500);
    } catch (e) {
      replaceChildren(jobBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  const run = async (name: string, mode: "offline" | "live") => {
    replaceChildren(jobBox, stateBox("loading", `starting ${mode} replay of ${name}…`));
    jobBox.scrollIntoView?.({ block: "nearest" });
    try {
      const r = await api.post<{ job_id: number }>("/api/replay/run", { scenario: name, mode });
      void watch(r.job_id);
    } catch (e) {
      replaceChildren(jobBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  const load = async () => {
    try {
      const items = (await api.get<{ items: ScenarioInfo[] }>("/api/replay/scenarios")).items;
      replaceChildren(listBox, h("h3", {}, "Scenarios"),
        h("table", { class: "data", "data-testid": "scenario-table" }, h("tbody", {}, ...items.map((sc) => h("tr", {},
          h("td", {}, h("strong", {}, sc.name), h("div", { class: "muted small" }, sc.description)),
          h("td", {}, `${sc.duration_s} s`),
          h("td", {}, h("button", { class: "btn", "data-scenario": sc.name, "aria-label": `Run ${sc.name} offline`,
                                    onclick: () => void run(sc.name, "offline") }, "Run offline"),
            h("button", { class: "btn", "aria-label": `Publish ${sc.name} live`,
                          onclick: () => void run(sc.name, "live") }, "Publish live")))))));
    } catch (e) {
      replaceChildren(listBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };
  replaceChildren(jobBox, h("h3", {}, "Result"), h("p", { class: "muted" }, "Run a scenario to see its result."));
  void load();
  return () => {
    alive = false;
    if (poll) clearTimeout(poll);
  };
}
