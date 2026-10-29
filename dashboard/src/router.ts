// Hash router: #/overview, #/trends, ...  Each page mounts into the main
// element and returns a cleanup function.

import { mountAudit } from "./pages/audit";
import { mountBaselines } from "./pages/baselines";
import { mountEvents } from "./pages/events";
import { mountExport } from "./pages/export";
import { mountIntegrity } from "./pages/integrity";
import { mountModels } from "./pages/models";
import { mountOverview } from "./pages/overview";
import { mountReplay } from "./pages/replay";
import { mountTrends } from "./pages/trends";

export type Mount = (root: HTMLElement) => () => void;

export const ROUTES: { path: string; label: string; mount: Mount }[] = [
  { path: "overview", label: "Overview", mount: mountOverview },
  { path: "trends", label: "Trends", mount: mountTrends },
  { path: "events", label: "Alerts & history", mount: mountEvents },
  { path: "integrity", label: "Data integrity", mount: mountIntegrity },
  { path: "baselines", label: "Baselines", mount: mountBaselines },
  { path: "models", label: "Methods", mount: mountModels },
  { path: "audit", label: "Audit", mount: mountAudit },
  { path: "replay", label: "Replay", mount: mountReplay },
  { path: "export", label: "Export", mount: mountExport },
];

export function currentPath(hash = location.hash): string {
  const p = hash.replace(/^#\/?/, "").split("?")[0];
  return ROUTES.some((r) => r.path === p) ? p : "overview";
}

export class Router {
  private cleanup: (() => void) | null = null;
  private current = "";

  constructor(private readonly main: HTMLElement, private readonly nav: HTMLElement) {}

  start(): void {
    window.addEventListener("hashchange", () => this.render());
    this.render();
  }

  render(force = false): void {
    const path = currentPath();
    const sameRoute = path === this.current;
    if (sameRoute && !force && !location.hash.includes("?")) return;
    this.cleanup?.();
    this.cleanup = null;
    this.current = path;
    while (this.main.firstChild) this.main.removeChild(this.main.firstChild);
    for (const a of Array.from(this.nav.querySelectorAll("a"))) {
      a.classList.toggle("active", a.getAttribute("href") === `#/${path}`);
      if (a.getAttribute("href") === `#/${path}`) a.setAttribute("aria-current", "page");
      else a.removeAttribute("aria-current");
    }
    const route = ROUTES.find((r) => r.path === path) ?? ROUTES[0];
    this.cleanup = route.mount(this.main);
    this.main.focus({ preventScroll: true });
  }
}
