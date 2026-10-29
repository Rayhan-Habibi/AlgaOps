// Minimal typed client for the local gateway API (same origin only).

export class ApiError extends Error {
  constructor(
    message: string,
    readonly status: number,
    readonly kind: "network" | "timeout" | "http" | "parse",
  ) {
    super(message);
    this.name = "ApiError";
  }
}

export interface ClientOptions {
  baseUrl?: string;
  timeoutMs?: number;
  fetchImpl?: typeof fetch;
  token?: () => string | null;
}

const TOKEN_KEY = "algaops.apiToken";

export function storedToken(): string | null {
  try {
    return globalThis.localStorage?.getItem(TOKEN_KEY) ?? null;
  } catch {
    return null;
  }
}

export function saveToken(token: string | null): void {
  try {
    if (token) globalThis.localStorage?.setItem(TOKEN_KEY, token);
    else globalThis.localStorage?.removeItem(TOKEN_KEY);
  } catch {
    /* storage unavailable: token is kept only for this page load */
  }
}

export type Query = Record<string, string | number | boolean | null | undefined>;

export function buildUrl(base: string, path: string, query?: Query): string {
  const qs = new URLSearchParams();
  for (const [k, v] of Object.entries(query ?? {})) {
    if (v !== undefined && v !== null && v !== "") qs.set(k, String(v));
  }
  const q = qs.toString();
  return `${base}${path}${q ? `?${q}` : ""}`;
}

export class ApiClient {
  private readonly base: string;
  private readonly timeoutMs: number;
  private readonly fetchImpl: typeof fetch;
  private readonly token: () => string | null;

  constructor(opts: ClientOptions = {}) {
    this.base = opts.baseUrl ?? "";
    this.timeoutMs = opts.timeoutMs ?? 10_000;
    this.fetchImpl = opts.fetchImpl ?? ((...a) => globalThis.fetch(...a));
    this.token = opts.token ?? storedToken;
  }

  async request<T>(method: string, path: string, opts: { query?: Query; body?: unknown } = {}): Promise<T> {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), this.timeoutMs);
    const headers: Record<string, string> = { Accept: "application/json" };
    if (opts.body !== undefined) headers["Content-Type"] = "application/json";
    const tok = this.token();
    if (tok && method !== "GET") headers["Authorization"] = `Bearer ${tok}`;
    let res: Response;
    try {
      res = await this.fetchImpl(buildUrl(this.base, path, opts.query), {
        method,
        headers,
        body: opts.body === undefined ? undefined : JSON.stringify(opts.body),
        signal: ctrl.signal,
        credentials: "same-origin",
        cache: "no-store",
      });
    } catch (e) {
      const aborted = e instanceof DOMException && e.name === "AbortError";
      throw new ApiError(aborted ? "gateway did not answer in time" : "gateway unreachable",
                         0, aborted ? "timeout" : "network");
    } finally {
      clearTimeout(timer);
    }
    const text = await res.text();
    let data: unknown = null;
    if (text) {
      try {
        data = JSON.parse(text);
      } catch {
        if (res.ok) throw new ApiError("invalid JSON from gateway", res.status, "parse");
      }
    }
    if (!res.ok) {
      const detail = (data && typeof data === "object" && "detail" in data)
        ? String((data as { detail: unknown }).detail) : res.statusText || `HTTP ${res.status}`;
      throw new ApiError(detail, res.status, "http");
    }
    return data as T;
  }

  get<T>(path: string, query?: Query): Promise<T> {
    return this.request<T>("GET", path, { query });
  }

  post<T>(path: string, body: unknown = {}): Promise<T> {
    return this.request<T>("POST", path, { body });
  }

  del<T>(path: string): Promise<T> {
    return this.request<T>("DELETE", path);
  }
}

export const api = new ApiClient();
