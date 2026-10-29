// Always-visible provenance banner: DEMO / SYNTHETIC vs LIVE HARDWARE.

import { dataSourceLabel } from "../util/format";
import { h } from "../util/dom";

const DETAIL: Record<string, string> = {
  demo: "Simulator scenarios — not measurements from a real pump, blower or PBR.",
  live: "Sensor-node data. Warnings are inspection requests, not validated diagnoses.",
  mixed: "Synthetic and hardware nodes are both active — check the source of each node.",
  nodata: "No telemetry received yet.",
};

export function dataSourceBanner(label: string | null | undefined,
                                 sources: Record<string, number> = {}): HTMLElement {
  const { text, cls } = dataSourceLabel(label ?? "NO DATA");
  const counts = Object.entries(sources).map(([k, v]) => `${k}: ${v}`).join(" · ");
  return h("div", { class: `source-banner ${cls}`, role: "note", "data-testid": "source-banner" },
           h("strong", {}, text),
           h("span", { class: "detail" }, DETAIL[cls]),
           counts ? h("span", { class: "counts" }, counts) : null);
}
