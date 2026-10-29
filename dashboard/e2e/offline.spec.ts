// Offline-first behaviour: no request leaves the gateway origin, the service
// worker keeps the app shell (never API data), and a lost gateway is visible.

import { expect, test } from "@playwright/test";

const ROUTES = ["overview", "trends", "events", "integrity", "baselines", "models", "audit", "replay", "export"];

test("the dashboard never requests anything outside the gateway origin", async ({ page, baseURL }) => {
  const foreign: string[] = [];
  page.on("request", (req) => {
    const u = req.url();
    if (!u.startsWith(baseURL!) && !u.startsWith("data:") && !u.startsWith("blob:")) foreign.push(u);
  });
  for (const r of ROUTES) {
    await page.goto(`/#/${r}`);
    await expect(page.locator("main h2")).toBeVisible();
  }
  expect(foreign).toEqual([]);
});

test("service worker precaches the built shell and never caches API responses", async ({ page }) => {
  await page.goto("/#/overview");
  await page.evaluate(async () => { await navigator.serviceWorker.ready; });
  for (const r of ["trends", "events"]) await page.goto(`/#/${r}`);
  const state = await page.evaluate(async () => {
    const html = await (await fetch("/index.html", { cache: "no-store" })).text();
    const assets = [...html.matchAll(/(?:src|href)="(\/assets\/[^"]+)"/g)].map((m) => m[1]);
    const missing = [];
    for (const a of assets) if (!(await caches.match(a))) missing.push(a);
    const api = [];
    for (const k of await caches.keys()) {
      for (const req of await (await caches.open(k)).keys()) {
        if (new URL(req.url).pathname.startsWith("/api/")) api.push(req.url);
      }
    }
    return { assets, missing, api };
  });
  expect(state.assets.length).toBeGreaterThan(0);
  expect(state.missing).toEqual([]);
  expect(state.api).toEqual([]);
});

test("without the gateway the cached shell loads and reports the failure", async ({ page, context }) => {
  await page.goto("/#/overview");
  await page.evaluate(async () => { await navigator.serviceWorker.ready; });
  await context.setOffline(true);
  try {
    await page.reload();
    await expect(page.getByText("AlgaOps Assurance").first()).toBeVisible();
    await expect(page.getByTestId("connection").locator(".pill.bad").first()).toBeVisible({ timeout: 15_000 });
    await expect(page.locator(".state-box.error").first()).toBeVisible({ timeout: 15_000 });
    await expect(page.getByTestId("node-card")).toHaveCount(0);   // no diagnostics invented offline
  } finally {
    await context.setOffline(false);
  }
});
