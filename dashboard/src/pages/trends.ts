// Trends: vibration / temperature / current / TrustScore over time with
// operating-mode bands and warning markers.

import type { Trend } from "../api/types";
import { ApiError, api } from "../api/client";
import { store } from "../state/store";
import { trendChart } from "../components/trend_chart";
import { modeLegend } from "../components/mode_bands";
import { h, replaceChildren, s, stateBox } from "../util/dom";

const METRICS: Record<string, { label: string; unit: string; digits: number }> = {
  vib_rms_g: { label: "Vibration RMS", unit: "g", digits: 3 },
  temp_c: { label: "Casing temperature", unit: "°C", digits: 1 },
  current_a: { label: "Motor current", unit: "A", digits: 2 },
  trust_score: { label: "TrustScore", unit: "score", digits: 0 },
};
const RANGES: Record<string, number> = { "15 min": 15 * 60e3, "1 h": 3600e3, "6 h": 6 * 3600e3,
                                         "24 h": 24 * 3600e3, "7 d": 7 * 24 * 3600e3 };

export function nodeSelect(onChange: () => void): HTMLSelectElement {
  const sel = h("select", { "aria-label": "sensor node" });
  let signature = "";
  const fill = (): boolean => {
    const st = store.get();
    const cur = store.selectedNode()?.node_id ?? "";
    const sig = `${cur}|${st.nodes.map((n) => n.node_id).join(",")}`;
    if (sig === signature) return false;
    const first = signature === "" || signature.startsWith("|");
    signature = sig;
    replaceChildren(sel, ...st.nodes.map((n) =>
      h("option", { value: n.node_id, selected: n.node_id === cur }, `${n.node_id} (${n.data_source})`)));
    return first && st.nodes.length > 0;
  };
  sel.addEventListener("change", () => {
    store.set({ selectedNode: sel.value });
    onChange();
  });
  fill();
  // Follow the node list (it may arrive after the page was mounted); the
  // subscription ends once the page has been replaced.
  const unsub = store.subscribe(() => {
    if (!sel.isConnected && signature !== "") {
      unsub();
      return;
    }
    if (fill()) onChange();
  });
  return sel;
}

export function mountTrends(root: HTMLElement): () => void {
  const charts = h("div", { class: "charts" });
  let range = "1 h";
  const rangeSel = h("select", { "aria-label": "time range" },
    ...Object.keys(RANGES).map((r) => h("option", { value: r, selected: r === range }, r)));
  rangeSel.addEventListener("change", () => { range = rangeSel.value; void load(); });
  const sel = nodeSelect(() => void load());
  root.append(h("h2", {}, "Trends"),
    h("div", { class: "toolbar" }, h("label", {}, "Node ", sel), h("label", {}, "Range ", rangeSel),
      s("svg", { viewBox: "0 0 280 14", class: "legend-svg" }, modeLegend()),
      h("span", { class: "legend-mk" }, h("span", { class: "sw mk-sensor" }), "CHECK SENSOR",
        h("span", { class: "sw mk-equip" }), "CHECK EQUIPMENT")),
    charts);

  let busy = false;
  let again = false;
  const load = async (): Promise<void> => {
    const node = store.selectedNode()?.node_id;
    if (!node) {
      replaceChildren(charts, stateBox("empty", "No node selected."));
      return;
    }
    if (busy) {
      again = true;   // a selection changed while loading: reload afterwards
      return;
    }
    busy = true;
    const until = Date.now();
    const since = until - RANGES[range];
    if (!charts.childElementCount) replaceChildren(charts, stateBox("loading", "loading trends…"));
    try {
      const results = await Promise.all(Object.keys(METRICS).map((m) =>
        api.get<Trend>("/api/history/trend", { node_id: node, metric: m, since_ms: since, until_ms: until,
                                               max_points: 400 })));
      replaceChildren(charts, ...results.map((t) => {
        const meta = METRICS[t.metric];
        return h("section", { class: "card chart", "data-testid": `chart-${t.metric}` },
          h("h3", {}, `${meta.label} (${meta.unit})`),
          trendChart(t, { unit: meta.unit, digits: meta.digits }));
      }));
    } catch (e) {
      replaceChildren(charts, stateBox("error", e instanceof ApiError ? e.message : "failed to load"));
    } finally {
      busy = false;
    }
    if (again) {
      again = false;
      await load();
    }
  };
  void load();
  const timer = setInterval(load, 10_000);
  return () => clearInterval(timer);
}
