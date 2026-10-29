// Service worker: keeps the app shell available when the browser has no
// internet (the gateway itself is local).  API responses are never cached:
// a technician must never see stale diagnostics presented as current.

const CACHE = "algaops-shell-v3";
const SHELL = ["/", "/index.html", "/icon.svg", "/manifest.webmanifest"];

interface ExtendableEvt extends Event { waitUntil(p: Promise<unknown>): void }
interface FetchEvt extends ExtendableEvt { request: Request; respondWith(r: Promise<Response> | Response): void }
interface SwScope {
  addEventListener(type: "install" | "activate", l: (e: ExtendableEvt) => void): void;
  addEventListener(type: "fetch", l: (e: FetchEvt) => void): void;
  skipWaiting(): Promise<void>;
  clients: { claim(): Promise<void> };
  location: Location;
}

const sw = self as unknown as SwScope;

// The hashed bundles are only known from the built index.html, so they are
// read from it: after a single visit the shell boots without the gateway.
async function precache(): Promise<void> {
  const cache = await caches.open(CACHE);
  await cache.addAll(SHELL);
  const html = (await (await cache.match("/index.html"))?.text()) ?? "";
  const assets = [...html.matchAll(/(?:src|href)="(\/assets\/[^"]+)"/g)].map((m) => m[1]);
  if (assets.length) await cache.addAll(assets);
}

sw.addEventListener("install", (e) => {
  e.waitUntil(precache().then(() => sw.skipWaiting()));
});

sw.addEventListener("activate", (e) => {
  e.waitUntil(caches.keys()
    .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => sw.clients.claim()));
});

sw.addEventListener("fetch", (e) => {
  const req = e.request;
  const url = new URL(req.url);
  if (req.method !== "GET" || url.origin !== sw.location.origin || url.pathname.startsWith("/api/")) {
    return;   // network only
  }
  if (req.mode === "navigate") {
    e.respondWith(fetch(req).then((res) => {
      if (res.ok && (res.headers.get("content-type") ?? "").includes("text/html")) {
        const copy = res.clone();   // keep the shell in step with the deployed build
        void caches.open(CACHE).then((c) => c.put("/index.html", copy));
      }
      return res;
    }).catch(async () => (await caches.match("/index.html")) ?? new Response("offline", { status: 503 })));
    return;
  }
  e.respondWith(caches.open(CACHE).then(async (cache) => {
    const hit = await cache.match(req);
    const net = fetch(req).then((res) => {
      if (res.ok && (url.pathname.startsWith("/assets/") || SHELL.includes(url.pathname))) {
        void cache.put(req, res.clone());
      }
      return res;
    });
    if (!hit) return net;
    net.catch(() => undefined);   // background refresh only; the cached copy answers
    return hit;
  }));
});
