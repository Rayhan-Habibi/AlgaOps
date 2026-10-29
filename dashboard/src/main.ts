// AlgaOps dashboard bootstrap: layout, periodic state refresh, live events,
// router and offline service worker.

import "./styles/main.css";
import type { GatewayStatus, NodeSummary } from "./api/types";
import { ApiError, api, saveToken, storedToken } from "./api/client";
import { EventStream } from "./api/events";
import { store } from "./state/store";
import { ROUTES, Router } from "./router";
import { dataSourceBanner } from "./components/data_source_banner";
import { connectionIndicator } from "./components/connection_indicator";
import { h, replaceChildren } from "./util/dom";

const banner = h("div", { class: "banner-slot" });
const conn = h("div", { class: "conn-slot" });
const nav = h("nav", { class: "nav", "aria-label": "sections" },
  ...ROUTES.map((r) => h("a", { href: `#/${r.path}` }, r.label)));
const main = h("main", { id: "main", tabindex: "-1" });
const staleNote = h("div", { class: "stale-note", role: "alert", hidden: true });

function settingsDialog(): HTMLDialogElement {
  const input = h("input", { type: "password", autocomplete: "off", value: storedToken() ?? "",
                             "aria-label": "API token" });
  const dlg = h("dialog", { class: "settings" },
    h("form", { method: "dialog" },
      h("h3", {}, "Gateway access"),
      h("p", { class: "muted" }, "Only needed when the gateway is exposed on the plant LAN with an API token. "
        + "The token is stored in this browser only."),
      h("label", {}, "API token ", input),
      h("menu", {},
        h("button", { value: "cancel", class: "btn" }, "Cancel"),
        h("button", { value: "save", class: "btn primary", onclick: () => saveToken(input.value.trim() || null) }, "Save"))));
  return dlg;
}

const dialog = settingsDialog();
document.getElementById("app")!.append(
  h("header", { class: "top" },
    h("div", { class: "brand" }, h("img", { src: "/icon.svg", alt: "", width: 28, height: 28 }),
      h("div", {}, h("strong", {}, "AlgaOps Assurance"), h("span", { class: "muted small" },
        "trust-gated edge diagnostics · read-only"))),
    conn,
    h("button", { class: "btn ghost", "aria-label": "settings", onclick: () => dialog.showModal() }, "⚙")),
  banner, staleNote, nav, main,
  h("footer", {}, "Inspection warnings for technicians only — no pump, blower or PBR control. ",
    "Thresholds are preliminary and not validated for industrial diagnosis."),
  dialog);

function renderChrome(): void {
  const s = store.get();
  replaceChildren(banner, dataSourceBanner(s.status?.data.label, s.status?.data.sources ?? {}));
  replaceChildren(conn, connectionIndicator(s));
  if (!s.apiOk && s.status) {
    staleNote.hidden = false;
    staleNote.textContent = `Gateway unreachable (${s.apiError}). Showing the last data received — it may be outdated.`;
  } else {
    staleNote.hidden = true;
  }
}

let refreshing = false;
export async function refresh(): Promise<void> {
  if (refreshing) return;
  refreshing = true;
  try {
    const [status, nodes] = await Promise.all([
      api.get<GatewayStatus>("/api/status"),
      api.get<{ nodes: NodeSummary[] }>("/api/nodes"),
    ]);
    store.set({ status, nodes: nodes.nodes, apiOk: true, apiError: null, lastRefreshMs: Date.now() });
  } catch (e) {
    store.set({ apiOk: false, apiError: e instanceof ApiError ? e.message : "unknown error" });
  } finally {
    refreshing = false;
  }
}

let pending: ReturnType<typeof setTimeout> | null = null;
function scheduleRefresh(delay = 400): void {
  if (pending) return;
  pending = setTimeout(() => { pending = null; void refresh(); }, delay);
}

store.subscribe(renderChrome);
renderChrome();
void refresh();
setInterval(refresh, 5000);

new EventStream({
  onState: (stream) => {
    store.set({ stream });
    if (stream === "open") scheduleRefresh(0);
  },
  onEvent: (e) => {
    if (e.type === "heartbeat" || e.type === "hello") return;
    store.pushEvent(e.type, e.data);
    scheduleRefresh();
  },
}).start();

new Router(main, nav).start();

if ("serviceWorker" in navigator && location.protocol !== "file:") {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch(() => undefined);
  });
}
