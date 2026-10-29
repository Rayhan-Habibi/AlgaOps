// Baselines per node and operating mode: capture from trusted live windows,
// activate, retire.  A captured baseline is a candidate until activated.

import type { Baseline, CaptureSession } from "../api/types";
import { ApiError, api } from "../api/client";
import { store } from "../state/store";
import { fmtNumber } from "../util/format";
import { fmtDateTime } from "../util/time";
import { h, replaceChildren, stateBox } from "../util/dom";

export function mountBaselines(root: HTMLElement): () => void {
  const list = h("section", { class: "card" });
  const capture = h("section", { class: "card" });
  root.append(h("h2", {}, "Baselines"),
    h("p", { class: "muted" }, "Baselines are built by the core only from windows it marked trusted and "
      + "classified into the selected mode. Re-capture on the real pump before relying on equipment warnings."),
    h("div", { class: "grid two" }, capture, list));

  const node = h("select", { "aria-label": "node" });
  const mode = h("select", { "aria-label": "mode" }, ...["STEADY", "STARTUP", "OFF"].map((m) => h("option", { value: m }, m)));
  const windows = h("input", { type: "number", min: 2, max: 100000, value: 300, "aria-label": "windows" });
  const activate = h("input", { type: "checkbox", "aria-label": "activate when complete" });
  const msg = h("p", { class: "muted", role: "status" });
  const sessions = h("div", {});
  const start = h("button", { class: "btn primary" }, "Start capture");
  start.addEventListener("click", async () => {
    msg.textContent = "starting…";
    try {
      await api.post("/api/baselines/capture", { node_id: node.value, mode: mode.value,
                                                 windows: Number(windows.value), activate: activate.checked });
      msg.textContent = "capture running: keep the asset in the selected mode and healthy";
      void load();
    } catch (e) {
      msg.textContent = e instanceof ApiError ? `failed: ${e.message}` : "failed";
    }
  });
  replaceChildren(capture, h("h3", {}, "Capture a baseline"),
    h("div", { class: "form" }, h("label", {}, "Node ", node), h("label", {}, "Mode ", mode),
      h("label", {}, "Windows ", windows), h("label", { class: "check" }, activate, " activate when complete"), start),
    msg, sessions);

  const load = async () => {
    replaceChildren(node, ...store.get().nodes.map((n) => h("option", { value: n.node_id }, n.node_id)));
    try {
      const r = await api.get<{ items: Baseline[]; capture: CaptureSession[]; min_samples: number }>("/api/baselines");
      replaceChildren(sessions, ...r.capture.map((c) => h("div", { class: "progress" },
        h("span", {}, `${c.node_id} ${c.mode}: ${c.accepted}/${c.target} windows (skipped ${c.skipped_untrusted} untrusted, ${c.skipped_other_mode} other mode)`),
        h("progress", { max: c.target, value: c.accepted }),
        h("button", { class: "btn", onclick: async () => { await api.del(`/api/baselines/capture/${c.node_id}`); void load(); } }, "Cancel"))));
      replaceChildren(list, h("h3", {}, `Stored baselines (minimum ${r.min_samples} samples)`),
        !r.items.length ? stateBox("empty", "No baselines yet: without one, STEADY windows are NOT EVALUATED.")
          : h("table", { class: "data", "data-testid": "baselines-table" },
            h("thead", {}, h("tr", {}, h("th", {}, "#"), h("th", {}, "Node"), h("th", {}, "Mode"), h("th", {}, "Status"),
              h("th", {}, "n"), h("th", {}, "Vibration mean ± std"), h("th", {}, "Created"), h("th", {}, ""))),
            h("tbody", {}, ...r.items.map((b) => h("tr", { class: b.status },
              h("td", {}, String(b.id)), h("td", {}, b.node_id), h("td", {}, b.mode), h("td", {}, b.status),
              h("td", {}, String(b.n)),
              h("td", {}, `${fmtNumber(b.stats.mean["vib_rms_g"], 4)} ± ${fmtNumber(b.stats.std["vib_rms_g"], 4)} g`),
              h("td", {}, fmtDateTime(b.created_ms)),
              h("td", {}, b.status !== "active"
                ? h("button", { class: "btn", onclick: async () => { await api.post(`/api/baselines/${b.id}/activate`); void load(); } }, "Activate")
                : h("button", { class: "btn", onclick: async () => { await api.post(`/api/baselines/${b.id}/retire`); void load(); } }, "Retire")))))));
    } catch (e) {
      replaceChildren(list, stateBox("error", e instanceof ApiError ? e.message : "failed"));
    }
  };
  void load();
  const timer = setInterval(load, 5000);
  return () => clearInterval(timer);
}
