// Recent windows as a strip of cells (one per received window) with gaps
// rendered explicitly, so lost packets are visible and never "filled in".

import { h, s } from "../util/dom";

export interface SeqItem { seq: number; boot_id: number; received_ms: number; flags: number; is_trusted: number }

const STALE = (1 << 10) | (1 << 11);

function cls(it: SeqItem): string {
  if (it.flags & (1 << 1)) return "dup";
  if (it.flags & (1 << 2)) return "ooo";
  if (it.flags & STALE) return "late";
  return it.is_trusted ? "valid" : "invalid";
}

export function sequenceGapChart(items: SeqItem[], maxCells = 240): HTMLElement {
  if (!items.length) return h("p", { class: "muted" }, "no windows in range");
  const recent = items.slice(-maxCells);
  const cells: { kind: string; label: string }[] = [];
  for (let i = 0; i < recent.length; i++) {
    const it = recent[i];
    const prev = recent[i - 1];
    if (prev && prev.boot_id === it.boot_id && it.seq > prev.seq + 1) {
      const gap = it.seq - prev.seq - 1;
      for (let k = 0; k < Math.min(gap, 20); k++) {
        cells.push({ kind: "lost", label: `lost seq ${prev.seq + 1 + k}` });
      }
      if (gap > 20) cells.push({ kind: "lost", label: `… ${gap - 20} more lost` });
    }
    if (prev && prev.boot_id !== it.boot_id) cells.push({ kind: "reboot", label: "node reboot" });
    cells.push({ kind: cls(it), label: `seq ${it.seq} (${cls(it)})` });
  }
  const size = 8;
  const perRow = 60;
  const rows = Math.ceil(cells.length / perRow);
  const svg = s("svg", { viewBox: `0 0 ${perRow * (size + 2)} ${rows * (size + 2)}`, class: "seqchart",
                         role: "img", "aria-label": "recent sequence numbers with gaps" });
  cells.forEach((c, i) => {
    const r = s("rect", { x: (i % perRow) * (size + 2), y: Math.floor(i / perRow) * (size + 2),
                          width: size, height: size, rx: 1.5, class: `sq ${c.kind}` });
    r.append(s("title", {}, c.label));
    svg.append(r);
  });
  return h("div", { class: "seqwrap" }, svg);
}
