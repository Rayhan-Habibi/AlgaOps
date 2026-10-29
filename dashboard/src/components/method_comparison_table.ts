// Method comparison table (labelled synthetic evaluation).

import type { ConfusionDict } from "../api/types";
import { fmtPercent } from "../util/format";
import { h } from "../util/dom";

export function confusionRow(label: string, c: ConfusionDict): HTMLTableRowElement {
  return h("tr", {}, h("th", { scope: "row" }, label),
    h("td", {}, fmtPercent(c.precision, 1)), h("td", {}, fmtPercent(c.recall, 1)),
    h("td", {}, fmtPercent(c.fpr, 2)), h("td", {}, fmtPercent(c.f1, 1)),
    h("td", {}, `${c.tp} / ${c.fp} / ${c.tn} / ${c.fn}`));
}

export function methodComparisonTable(rows: Record<string, ConfusionDict>, caption: string): HTMLElement {
  return h("table", { class: "data", "data-testid": "method-table" },
    h("caption", {}, caption),
    h("thead", {}, h("tr", {}, h("th", {}, "Method"), h("th", {}, "Precision"), h("th", {}, "Recall"),
      h("th", {}, "False-positive rate"), h("th", {}, "F1"), h("th", {}, "TP / FP / TN / FN"))),
    h("tbody", {}, ...Object.entries(rows).map(([m, c]) => confusionRow(m, c))));
}
