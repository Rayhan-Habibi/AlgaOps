// SVG trend chart: min-max band, mean line, operating-mode bands and
// CHECK_SENSOR / CHECK_EQUIPMENT markers.  No charting library needed.

import type { Trend } from "../api/types";
import { fmtTime } from "../util/time";
import { s } from "../util/dom";
import { modeBands } from "./mode_bands";

export interface ChartOptions {
  width?: number;
  height?: number;
  unit?: string;
  digits?: number;
}

export function niceTicks(min: number, max: number, count = 5): number[] {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return [];
  if (min === max) {
    const d = Math.abs(min) * 0.1 || 1;
    min -= d;
    max += d;
  }
  const raw = (max - min) / count;
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((st) => raw <= st) ?? 10 * mag;
  const start = Math.ceil(min / step) * step;
  const out: number[] = [];
  for (let v = start; v <= max + step * 1e-9; v += step) out.push(Number(v.toPrecision(12)));
  return out;
}

export function trendChart(t: Trend, opts: ChartOptions = {}): SVGSVGElement {
  const W = opts.width ?? 900;
  const H = opts.height ?? 300;
  const pad = { l: 56, r: 16, t: 22, b: 30 };
  const iw = W - pad.l - pad.r;
  const ih = H - pad.t - pad.b;
  const svg = s("svg", { viewBox: `0 0 ${W} ${H}`, class: "trend", role: "img",
                         "aria-label": `${t.metric} trend for ${t.node_id}` });
  const pts = t.points.filter((p) => p.mean !== null && p.min !== null && p.max !== null);
  if (!pts.length) {
    svg.append(s("text", { x: W / 2, y: H / 2, "text-anchor": "middle", class: "empty" },
                 "no data in this time range"));
    return svg;
  }
  let lo = Math.min(...pts.map((p) => p.min as number));
  let hi = Math.max(...pts.map((p) => p.max as number));
  if (lo === hi) { lo -= Math.abs(lo) * 0.05 || 0.5; hi += Math.abs(hi) * 0.05 || 0.5; }
  const span = hi - lo;
  lo -= span * 0.08;
  hi += span * 0.08;
  const x = (ms: number) => pad.l + ((ms - t.since_ms) / Math.max(1, t.until_ms - t.since_ms)) * iw;
  const y = (v: number) => pad.t + ih - ((v - lo) / (hi - lo)) * ih;

  svg.append(modeBands(t.modes, x, pad.t, ih));
  const grid = s("g", { class: "grid" });
  for (const v of niceTicks(lo, hi)) {
    grid.append(s("line", { x1: pad.l, x2: W - pad.r, y1: y(v), y2: y(v) }),
                s("text", { x: pad.l - 6, y: y(v) + 4, "text-anchor": "end" },
                  v.toFixed(opts.digits ?? 3)));
  }
  const ticks = 6;
  for (let i = 0; i <= ticks; i++) {
    const ms = t.since_ms + ((t.until_ms - t.since_ms) * i) / ticks;
    grid.append(s("text", { x: x(ms), y: H - 8, "text-anchor": "middle" }, fmtTime(ms)));
  }
  svg.append(grid);

  const upper = pts.map((p) => `${x(p.t).toFixed(1)},${y(p.max as number).toFixed(1)}`);
  const lower = pts.slice().reverse().map((p) => `${x(p.t).toFixed(1)},${y(p.min as number).toFixed(1)}`);
  svg.append(s("polygon", { points: [...upper, ...lower].join(" "), class: "band" }));
  const d = pts.map((p, i) => `${i ? "L" : "M"}${x(p.t).toFixed(1)},${y(p.mean as number).toFixed(1)}`).join("");
  svg.append(s("path", { d, class: "line" }));

  const marks = s("g", { class: "markers" });
  for (const m of t.markers) {
    const r = s("rect", { x: x(m.since) - 1, y: pad.t - 14, width: Math.max(3, x(m.until) - x(m.since) + 2),
                          height: 8, class: m.outcome === "CHECK_SENSOR" ? "mk-sensor" : "mk-equip" });
    r.append(s("title", {}, `${m.outcome} ${fmtTime(m.since)}–${fmtTime(m.until)}`));
    marks.append(r);
  }
  svg.append(marks);
  if (opts.unit) svg.append(s("text", { x: 6, y: 14, class: "unit" }, opts.unit));
  return svg;
}
