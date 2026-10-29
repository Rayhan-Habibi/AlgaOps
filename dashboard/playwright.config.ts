import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "@playwright/test";

// The e2e suite runs against the real gateway (API + built dashboard) started
// with a freshly replayed synthetic dataset by tests/e2e/dashboard_server.py.
// It uses an already installed Edge or Chrome, so no browser download is needed.
const port = Number(process.env.ALGAOPS_E2E_PORT ?? 8099);
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const python = process.env.ALGAOPS_PYTHON
  ?? path.join(root, ".venv", process.platform === "win32" ? "Scripts/python.exe" : "bin/python");
const server = path.join(root, "tests", "e2e", "dashboard_server.py");

export default defineConfig({
  testDir: "e2e",
  timeout: 60_000,
  retries: 0,
  reporter: [["list"]],
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    channel: process.env.ALGAOPS_BROWSER_CHANNEL ?? "msedge",
    headless: true,
    serviceWorkers: "allow",
  },
  webServer: {
    command: `"${python}" "${server}" --port ${port}`,
    url: `http://127.0.0.1:${port}/api/health`,
    timeout: 120_000,
    reuseExistingServer: false,
    stdout: "pipe",
  },
});
