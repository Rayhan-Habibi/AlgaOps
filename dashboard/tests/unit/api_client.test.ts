import { describe, expect, it } from "vitest";
import { ApiClient, ApiError, buildUrl } from "../../src/api/client";

function fakeFetch(status: number, body: string, delayMs = 0): typeof fetch {
  return (async (_url: RequestInfo | URL, init?: RequestInit) => {
    if (delayMs) {
      await new Promise((resolve, reject) => {
        const t = setTimeout(resolve, delayMs);
        init?.signal?.addEventListener("abort", () => {
          clearTimeout(t);
          reject(new DOMException("aborted", "AbortError"));
        });
      });
    }
    return new Response(body, { status, headers: { "Content-Type": "application/json" } });
  }) as typeof fetch;
}

describe("buildUrl", () => {
  it("drops empty query values", () => {
    expect(buildUrl("", "/api/x", { a: 1, b: "", c: null, d: undefined, e: "y z" })).toBe("/api/x?a=1&e=y+z");
  });
});

describe("ApiClient", () => {
  it("parses JSON responses", async () => {
    const c = new ApiClient({ fetchImpl: fakeFetch(200, '{"status":"ok"}'), token: () => null });
    expect(await c.get<{ status: string }>("/api/health")).toEqual({ status: "ok" });
  });

  it("maps HTTP errors with the gateway detail", async () => {
    const c = new ApiClient({ fetchImpl: fakeFetch(404, '{"detail":"node x not found"}'), token: () => null });
    await expect(c.get("/api/nodes/x")).rejects.toMatchObject({ status: 404, kind: "http",
                                                                 message: "node x not found" });
  });

  it("reports network failures and timeouts distinctly", async () => {
    const down = new ApiClient({ fetchImpl: (() => Promise.reject(new TypeError("fail"))) as typeof fetch,
                                 token: () => null });
    await expect(down.get("/api/status")).rejects.toMatchObject({ kind: "network" });
    const slow = new ApiClient({ fetchImpl: fakeFetch(200, "{}", 200), timeoutMs: 20, token: () => null });
    await expect(slow.get("/api/status")).rejects.toBeInstanceOf(ApiError);
    await expect(slow.get("/api/status")).rejects.toMatchObject({ kind: "timeout" });
  });

  it("rejects invalid JSON bodies", async () => {
    const c = new ApiClient({ fetchImpl: fakeFetch(200, "<html>"), token: () => null });
    await expect(c.get("/api/status")).rejects.toMatchObject({ kind: "parse" });
  });

  it("sends the token only on state-changing requests", async () => {
    const seen: (string | null)[] = [];
    const f = (async (_u: RequestInfo | URL, init?: RequestInit) => {
      seen.push((init?.headers as Record<string, string>)["Authorization"] ?? null);
      return new Response("{}", { status: 200 });
    }) as typeof fetch;
    const c = new ApiClient({ fetchImpl: f, token: () => "secret-token-123456" });
    await c.get("/api/status");
    await c.post("/api/audit/verify");
    expect(seen).toEqual([null, "Bearer secret-token-123456"]);
  });
});
