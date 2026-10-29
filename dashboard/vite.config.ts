import { defineConfig } from "vitest/config";

// Everything the dashboard needs is bundled locally: no CDN, no web fonts,
// no external requests at runtime.  The service worker is emitted as /sw.js
// so that its scope covers the whole app.
export default defineConfig({
  base: "/",
  build: {
    outDir: "dist",
    emptyOutDir: true,
    sourcemap: false,
    assetsInlineLimit: 0,
    rollupOptions: {
      input: { main: "index.html", sw: "src/sw.ts" },
      output: {
        entryFileNames: (chunk) => (chunk.name === "sw" ? "sw.js" : "assets/[name]-[hash].js"),
      },
    },
  },
  server: {
    port: 5173,
    proxy: { "/api": { target: "http://127.0.0.1:8080", changeOrigin: false } },
  },
  test: {
    environment: "jsdom",
    include: ["tests/unit/**/*.test.ts"],
  },
});
