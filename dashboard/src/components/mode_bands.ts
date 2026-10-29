// OFF / STARTUP / STEADY background bands for time-series charts.

import type { Mode } from "../api/types";
import { s } from "../util/dom";

export const MODE_CLASS: Record<Mode, string> = {
  UNKNOWN: "mode-unknown",
  OFF: "mode-off",
  STARTUP: "mode-startup",
  STEADY: "mode-steady",
};

export function modeBands(modes: { mode: Mode; since: number; until: number }[],
                          x: (t: number) => number, top: number, height: number,
                          minWidth = 2): SVGGElement {
  const g = s("g", { class: "mode-bands" });
  for (const m of modes) {
    const x0 = x(m.since);
    const w = Math.max(minWidth, x(m.until) - x0);
    const r = s("rect", { x: x0, y: top, width: w, height, class: MODE_CLASS[m.mode] ?? "mode-unknown" });
    r.append(s("title", {}, m.mode));
    g.append(r);
  }
  return g;
}

export function modeLegend(): SVGGElement {
  const g = s("g", { class: "legend" });
  ["OFF", "STARTUP", "STEADY"].forEach((m, i) => {
    g.append(s("rect", { x: i * 90, y: 0, width: 12, height: 12, class: MODE_CLASS[m as Mode] }),
             s("text", { x: i * 90 + 16, y: 10 }, m));
  });
  return g;
}
