// TrustScore gauge and TrustGuard flag chips.

import { INFORMATIONAL_FLAGS, flagText } from "../util/format";
import { h, s } from "../util/dom";

export function trustGauge(score: number | null | undefined, trusted: boolean | undefined): SVGElement {
  const v = Math.max(0, Math.min(100, score ?? 0));
  const r = 26;
  const c = 2 * Math.PI * r;
  const svg = s("svg", { viewBox: "0 0 64 64", class: "gauge", role: "img",
                         "aria-label": `TrustScore ${v}` });
  svg.append(
    s("circle", { cx: 32, cy: 32, r, class: "gauge-bg" }),
    s("circle", { cx: 32, cy: 32, r, class: `gauge-fg ${trusted ? "ok" : "bad"}`,
                  "stroke-dasharray": `${(c * v) / 100} ${c}`, transform: "rotate(-90 32 32)" }),
    s("text", { x: 32, y: 37, "text-anchor": "middle", class: "gauge-text" }, String(v)),
  );
  return svg;
}

export function flagChips(flags: string[]): HTMLElement {
  if (!flags.length) return h("span", { class: "chip ok" }, "no integrity flags");
  return h("span", { class: "chips" }, ...flags.map((f) =>
    h("span", { class: `chip ${INFORMATIONAL_FLAGS.has(f) ? "info" : "block"}`, title: flagText(f) },
      f)));
}

export function trustBlock(score: number | undefined, trusted: boolean | undefined,
                           flags: string[]): HTMLElement {
  return h("div", { class: "trust" }, trustGauge(score, trusted),
           h("div", {}, h("div", { class: "muted" }, trusted ? "Data trusted" : "Data NOT trusted"),
             flagChips(flags)));
}
