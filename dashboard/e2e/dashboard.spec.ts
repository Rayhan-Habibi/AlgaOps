// End-to-end checks of the dashboard against the real gateway API, on a
// database replayed from synthetic scenarios (tests/e2e/dashboard_server.py):
//   sim-pump-01 normal_steady, sim-pump-02 stuck_vibration,
//   sim-pump-03 equipment_vibration_anomaly.
// Shortly after start-up the replayed nodes fall silent, so their latest
// outcome becomes CHECK SENSOR (stream timeout); the assertions below hold
// before and after that.

import { execFileSync } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { expect, test } from "@playwright/test";

const ROOT = fileURLToPath(new URL("../..", import.meta.url));
const PYTHON = process.env.ALGAOPS_PYTHON
  ?? path.join(ROOT, ".venv", process.platform === "win32" ? "Scripts/python.exe" : "bin/python");
const OUTCOMES = ["NORMAL", "CHECK SENSOR", "CHECK EQUIPMENT", "NOT EVALUATED"];
const ROUTES = ["overview", "trends", "events", "integrity", "baselines", "models", "audit", "replay", "export"];

test("overview labels the data as synthetic and shows every node", async ({ page }) => {
  await page.goto("/#/overview");
  await expect(page.getByTestId("source-banner")).toContainText("DEMO / SYNTHETIC");
  await expect(page.getByTestId("node-card")).toHaveCount(3);
  for (const id of ["sim-pump-01", "sim-pump-02", "sim-pump-03"]) {
    const card = page.locator(`[data-node="${id}"]`);
    await expect(card).toContainText("SIMULATOR");
    expect(OUTCOMES).toContain((await card.getByTestId("outcome").textContent())?.trim());
  }
  await expect(page.getByTestId("gateway-strip")).toContainText("read-only");
});

test("trust gate: only the equipment-fault node is ever CHECK EQUIPMENT", async ({ page }) => {
  await page.goto("/#/events");
  const table = page.getByTestId("diag-table");
  await page.getByLabel("outcome filter").selectOption("CHECK_EQUIPMENT");
  await expect.poll(async () => [...new Set(await table.locator("tbody td:nth-child(5)").allTextContents())])
    .toEqual(["CHECK EQUIPMENT"]);
  expect([...new Set(await table.locator("tbody td:nth-child(3)").allTextContents())]).toEqual(["sim-pump-03"]);

  // The stuck accelerometer is reported as a sensor problem, with its flag.
  await page.getByLabel("outcome filter").selectOption("CHECK_SENSOR");
  const stuck = table.locator("tbody tr", { hasText: "sim-pump-02" }).filter({ hasText: "VIB_STUCK" });
  await expect(stuck.first()).toBeVisible();
  await stuck.first().getByRole("link").click();
  await expect(page.getByTestId("decision-detail")).toContainText("VIB_STUCK");
});

test("alert detail accepts technician feedback and the feedback is audited", async ({ page }) => {
  await page.goto("/#/events");
  const row = page.getByTestId("alerts-table").locator("tbody tr", { hasText: "sim-pump-03" })
    .filter({ hasText: "CHECK EQUIPMENT" });
  await row.first().getByRole("link").click();
  const fb = page.getByTestId("feedback");
  await fb.getByRole("button", { name: "True alarm" }).click();
  await expect(fb.getByRole("status")).toContainText("initials");          // technician is required
  await fb.getByLabel("technician").fill("QA");
  await fb.getByLabel("note").fill("e2e: imbalance confirmed on rig");
  await fb.getByRole("button", { name: "True alarm" }).click();
  await expect(page.getByText(/true_alarm by QA/)).toBeVisible();

  await page.goto("/#/audit");
  await expect(page.getByTestId("chain-status")).toBeVisible();
  await expect(page.locator("table.data")).toContainText("feedback.recorded");
});

test("trends draw all four series with mode bands for the selected node", async ({ page }) => {
  await page.goto("/#/trends");
  await page.getByLabel("sensor node").selectOption("sim-pump-03");
  await page.getByLabel("time range").selectOption("15 min");
  for (const m of ["vib_rms_g", "temp_c", "current_a", "trust_score"]) {
    await expect(page.getByTestId(`chart-${m}`).locator("svg.trend path.line")).toHaveCount(1);
  }
  await expect(page.getByTestId("chart-vib_rms_g").locator(".markers .mk-equip").first()).toBeAttached();
});

test("data integrity accounts for every packet of the selected node", async ({ page }) => {
  await page.goto("/#/integrity");
  await page.getByLabel("sensor node").selectOption("sim-pump-02");
  const legend = page.getByTestId("packet-legend");
  await expect(legend).toContainText("valid");
  await expect(legend).toContainText("duplicate");
  await expect(page.getByText(/Completeness:/)).toBeVisible();
});

test("audit chain verifies on demand", async ({ page }) => {
  await page.goto("/#/audit");
  await expect(page.getByTestId("chain-status")).toContainText("Ascon-Hash256");
  await page.getByTestId("verify-btn").click();
  const result = page.getByTestId("verify-result");
  await expect(result).toContainText("CHAIN VERIFIED");
  // The status card refreshes after a verification; the report must survive it.
  await expect(page.getByTestId("chain-status")).toContainText(/OK at #\d+, just now/);
  await expect(result).toContainText("CHAIN VERIFIED");
  await expect(result).toContainText("not tamper-proof");   // the limitation is always stated
});

test("evidence bundle downloads and verifies with the standalone tool", async ({ page }) => {
  await page.goto("/#/export");
  await page.getByTestId("export-btn").click();
  const result = page.getByTestId("export-result");
  await expect(result).toContainText("chain verified");
  await expect(result).toContainText("DEMO / SYNTHETIC");
  const [download] = await Promise.all([page.waitForEvent("download"), result.getByRole("link").click()]);
  const file = path.join(mkdtempSync(path.join(tmpdir(), "algaops-e2e-")), download.suggestedFilename());
  await download.saveAs(file);
  const out = execFileSync(PYTHON, ["-I", path.join(ROOT, "tools", "verify_evidence.py"), file], { encoding: "utf8" });
  expect(out).toContain("EVIDENCE VERIFIED");
});

test("offline scenario replay reports outcome against ground truth", async ({ page }) => {
  await page.goto("/#/replay");
  await page.getByRole("button", { name: "Run missing_packet offline" }).click();
  const result = page.getByTestId("replay-result");
  await expect(result).toContainText("Matches expectation", { timeout: 30_000 });
  await expect(result).toContainText("CHECK_EQUIPMENT on faulty data: 0");
});

test("labelled evaluation runs and reports synthetic metrics", async ({ page }) => {
  await page.goto("/#/models");
  await page.getByTestId("run-eval").click();
  await expect(page.getByText(/scenarios · [\d.]+ s/)).toBeVisible({ timeout: 45_000 });
  await expect(page.getByTestId("method-table").first()).toBeVisible();
  await expect(page.locator("main")).toContainText("synthetic");
});

test("API failures are shown explicitly, never as current data", async ({ page }) => {
  await page.goto("/#/overview");
  await expect(page.getByTestId("node-card")).toHaveCount(3);
  await page.route("**/api/**", (r) => r.abort("connectionrefused"));
  const note = page.locator(".stale-note");
  await expect(note).toBeVisible({ timeout: 15_000 });
  await expect(note).toContainText("Gateway unreachable");
  await expect(note).toContainText("may be outdated");
  await expect(page.getByTestId("connection").locator(".pill.bad").first()).toBeVisible();
});

test("empty and error states are rendered", async ({ page }) => {
  await page.route((u) => u.pathname === "/api/alerts", (r) => r.fulfill({ json: { items: [] } }));
  await page.route((u) => u.pathname === "/api/diagnostics", (r) => r.fulfill({ status: 500, json: { detail: "boom" } }));
  await page.goto("/#/events");
  await expect(page.getByText("No alerts recorded.")).toBeVisible();
  await expect(page.locator(".state-box.error").first()).toBeVisible();
});

test("every page fits a phone screen without horizontal scrolling", async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 812 });
  for (const r of ROUTES) {
    await page.goto(`/#/${r}`);
    await expect(page.locator("main h2")).toBeVisible();
    await page.waitForTimeout(300);
    const overflow = await page.evaluate(() => document.documentElement.scrollWidth - document.documentElement.clientWidth);
    expect(overflow, `${r} overflows by ${overflow}px`).toBeLessThanOrEqual(1);
  }
});
