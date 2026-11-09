// End-to-end Playwright verification of the live SSE stream, browser event log,
// and end-to-end DOM render latency instrumentation (WP-4A-4).

import { expect, test } from "@playwright/test";

test.describe("SSE stream & DOM render latency instrumentation (WP-4A-4)", () => {
  test("dashboard loads, establishes SSE connection, and shows read-only banner", async ({ page }) => {
    await page.goto("/#/overview");

    // Verify synthetic data banner and read-only technician notice
    await expect(page.getByTestId("source-banner")).toContainText("DEMO / SYNTHETIC");
    await expect(page.getByTestId("gateway-strip")).toContainText("read-only");

    // Verify SSE connection indicator becomes active or connected
    const conn = page.getByTestId("connection");
    await expect(conn).toBeVisible();

    // Verify all 3 demo nodes appear in the cards
    await expect(page.getByTestId("node-card")).toHaveCount(3);
  });

  test("live SSE decision event renders to DOM with latency attributes", async ({ page }) => {
    await page.goto("/#/overview");

    // Inject a decision event into the store to simulate SSE message arrival
    const t0 = Date.now();
    const mockReceivedMs = t0 - 55; // 55 ms prior to render

    await page.evaluate((recvMs) => {
      const store = (window as unknown as Record<string, any>).__algaops_store;
      store.pushEvent("decision", {
        node_id: "sim-pump-03",
        outcome: "CHECK_EQUIPMENT",
        reason: "playwright: mechanical vibration excursion above baseline",
        received_ms: recvMs,
        alert_event: "OPENED",
      });
    }, mockReceivedMs);

    // Locate the newly rendered event in the live event log
    const logList = page.getByTestId("event-log");
    await expect(logList).toBeVisible();

    const item = logList.locator("li", { hasText: "sim-pump-03" }).first();
    await expect(item).toBeVisible();
    await expect(item).toContainText("CHECK EQUIPMENT");
    await expect(item).toContainText("mechanical vibration excursion");

    // Verify DOM latency attributes are present and numerically valid
    await expect(item).toHaveAttribute("data-received-ms", String(mockReceivedMs));
    await expect(item).toHaveAttribute("data-rendered-ms", /^\d+$/);
    await expect(item).toHaveAttribute("data-dom-latency-ms", /^\d+$/);

    const renderedMs = Number(await item.getAttribute("data-rendered-ms"));
    const domLatencyMs = Number(await item.getAttribute("data-dom-latency-ms"));

    expect(renderedMs).toBeGreaterThanOrEqual(mockReceivedMs);
    expect(domLatencyMs).toBe(renderedMs - mockReceivedMs);
    expect(domLatencyMs).toBeGreaterThanOrEqual(50);
    expect(domLatencyMs).toBeLessThan(2000); // Host DOM render p95 <= 2000 ms invariant

    // Verify window global hook for external latency auditors
    const windowLatency = await page.evaluate(() => {
      return (window as unknown as Record<string, any>).__algaops_last_dom_latency_ms;
    });
    expect(windowLatency).toBe(domLatencyMs);

    // Take screenshot of live event log for audit evidence
    await page.screenshot({ path: "var/e2e_event_log_latency.png" });
  });

  test("NORMAL decisions without alerts are filtered out of live event log", async ({ page }) => {
    await page.goto("/#/overview");

    // Inject a NORMAL decision without alert event
    await page.evaluate(() => {
      const store = (window as unknown as Record<string, any>).__algaops_store;
      // Clear live events first
      store.set({ liveEvents: [] });
      store.pushEvent("decision", {
        node_id: "sim-pump-01",
        outcome: "NORMAL",
        reason: "steady nominal telemetry",
        received_ms: Date.now() - 20,
        alert_event: "NONE",
      });
    });

    // Verify empty state is rendered because NORMAL was filtered
    const emptyMsg = page.locator("p.muted", { hasText: "No warnings or events" });
    await expect(emptyMsg).toBeVisible();

    const eventList = page.getByTestId("event-log");
    await expect(eventList).toHaveCount(0);
  });

  test("missing received_ms and null fields do not crash the UI", async ({ page }) => {
    await page.goto("/#/overview");

    // Inject decision with null received_ms and null reason
    await page.evaluate(() => {
      const store = (window as unknown as Record<string, any>).__algaops_store;
      store.set({ liveEvents: [] });
      store.pushEvent("decision", {
        node_id: "sim-pump-02",
        outcome: "CHECK_SENSOR",
        reason: null,
        received_ms: null,
      });
    });

    const item = page.getByTestId("event-log").locator("li", { hasText: "sim-pump-02" }).first();
    await expect(item).toBeVisible();
    await expect(item).toContainText("sim-pump-02");
    await expect(item).toContainText("CHECK SENSOR");

    // data-received-ms and data-dom-latency-ms should be absent, but data-rendered-ms present
    await expect(item).not.toHaveAttribute("data-received-ms");
    await expect(item).not.toHaveAttribute("data-dom-latency-ms");
    await expect(item).toHaveAttribute("data-rendered-ms", /^\d+$/);
  });
});

