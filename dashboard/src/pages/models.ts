// Analytic method comparison: flag rates on live data (unlabelled) and the
// labelled SYNTHETIC evaluation (precision / recall / FPR / latency).

import type { EvaluationReport, MethodComparison } from "../api/types";
import { ApiError, api } from "../api/client";
import { methodComparisonTable } from "../components/method_comparison_table";
import { fmtPercent } from "../util/format";
import { fmtDateTime } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";

export function mountModels(root: HTMLElement): () => void {
  const live = h("section", { class: "card" });
  const evalBox = h("section", { class: "card" });
  root.append(h("h2", {}, "Analytic methods"), live, evalBox);

  const loadLive = async () => {
    try {
      const r = await api.get<MethodComparison>("/api/analytics/methods");
      const modes = Object.entries(r.per_mode);
      replaceChildren(live, h("h3", {}, "Live data: how often each method flags trusted windows"),
        h("p", { class: "muted" }, r.note + " Only the primary method of a mode decides; the others are logged for comparison."),
        !modes.length ? stateBox("empty", "No trusted windows yet.")
          : h("table", { class: "data" }, h("thead", {}, h("tr", {}, h("th", {}, "Mode"), h("th", {}, "Method"),
            h("th", {}, "Evaluated"), h("th", {}, "Unavailable"), h("th", {}, "Flagged"), h("th", {}, "Flag rate"))),
            h("tbody", {}, ...modes.flatMap(([mode, ms]) => Object.entries(ms).map(([m, v]) => h("tr", {},
              h("td", {}, mode), h("td", {}, m), h("td", {}, String(v.evaluated)), h("td", {}, String(v.unavailable)),
              h("td", {}, String(v.flagged)), h("td", {}, fmtPercent(v.flag_rate, 2))))))));
    } catch (e) {
      replaceChildren(live, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };

  const runBtn = h("button", { class: "btn primary", "data-testid": "run-eval" }, "Run labelled evaluation");
  runBtn.addEventListener("click", async () => {
    try {
      await api.post("/api/analytics/evaluation", { full: false });
      void loadEval();
    } catch (e) {
      replaceChildren(evalBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  });
  const loadEval = async () => {
    try {
      const r = await api.get<{ state: string; error: string | null; report: EvaluationReport | null }>("/api/analytics/evaluation");
      const rep = r.report;
      const head = [h("h3", {}, "Labelled evaluation on synthetic scenarios"),
        h("p", { class: "disclaimer" }, rep?.disclaimer ?? "Runs every simulator scenario through the real pipeline "
          + "with ground truth kept outside the payloads. Results are synthetic, not field performance."),
        h("div", { class: "toolbar" }, runBtn, h("span", { class: "muted" }, `state: ${r.state}${r.error ? ` (${r.error})` : ""}`))];
      if (!rep) {
        replaceChildren(evalBox, ...head, stateBox("empty", "No evaluation report yet."));
      } else {
        const o = rep.overall;
        replaceChildren(evalBox, ...head,
          h("p", {}, `Generated ${fmtDateTime(rep.generated_ms)} · engine ${rep.engine_version} · `
            + `${rep.scenarios.length} scenarios · ${rep.wall_time_s} s`),
          methodComparisonTable({ "TrustGuard (CHECK_SENSOR on sensor/transport faults)": o.trust_detection,
                                  "System CHECK_EQUIPMENT on equipment faults": o.equipment_detection },
                                "Trust gate and attribution (all scenarios, per message)"),
          methodComparisonTable(o.methods_steady, "Equipment methods on trusted STEADY windows (shadow comparison)"),
          h("p", {}, `CHECK_EQUIPMENT raised on sensor/transport faults: ${o.attribution_errors} · `
            + `on untrusted data: ${o.untrusted_check_equipment}`),
          h("table", { class: "data" }, h("thead", {}, h("tr", {}, h("th", {}, "Scenario"), h("th", {}, "Messages"),
            h("th", {}, "Match expected"), h("th", {}, "Fault events detected"), h("th", {}, "Latency (s)"),
            h("th", {}, "Alerts"))),
            h("tbody", {}, ...rep.scenarios.map((sc) => h("tr", {}, h("td", { title: sc.description }, sc.name),
              h("td", {}, String(sc.messages)), h("td", {}, fmtPercent(sc.expected_match_rate, 1)),
              h("td", {}, `${sc.fault_events.filter((e) => e.detected).length}/${sc.fault_events.length}`),
              h("td", {}, sc.fault_events.map((e) => e.latency_s ?? "—").join(", ") || "—"),
              h("td", {}, Object.entries(sc.alerts_opened).map(([k, v]) => `${k}:${v}`).join(" ") || "—"))))));
      }
      if (r.state === "running") setTimeout(loadEval, 2000);
    } catch (e) {
      replaceChildren(evalBox, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };
  void loadLive();
  void loadEval();
  return () => undefined;
}
