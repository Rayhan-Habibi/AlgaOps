// Server-Sent Events client with reconnect back-off.  The REST API stays the
// source of truth: after every (re)connect the app refreshes its state.

export type StreamState = "connecting" | "open" | "closed";

export interface GatewayEvent {
  type: string;
  data: Record<string, unknown>;
}

export interface EventStreamOptions {
  url?: string;
  onEvent: (e: GatewayEvent) => void;
  onState: (s: StreamState) => void;
  factory?: (url: string) => EventSource;
}

const TYPES = ["decision", "alert", "node", "audit", "baseline", "packet", "gateway", "heartbeat", "hello"];

export class EventStream {
  private es: EventSource | null = null;
  private retryMs = 1000;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private stopped = false;

  constructor(private readonly opts: EventStreamOptions) {}

  start(): void {
    this.stopped = false;
    this.connect();
  }

  stop(): void {
    this.stopped = true;
    if (this.timer) clearTimeout(this.timer);
    this.es?.close();
    this.es = null;
    this.opts.onState("closed");
  }

  private connect(): void {
    if (this.stopped) return;
    this.opts.onState("connecting");
    const make = this.opts.factory ?? ((u: string) => new EventSource(u));
    const es = make(this.opts.url ?? "/api/events");
    this.es = es;
    es.onopen = () => {
      this.retryMs = 1000;
      this.opts.onState("open");
    };
    for (const t of TYPES) {
      es.addEventListener(t, (ev) => {
        try {
          this.opts.onEvent({ type: t, data: JSON.parse((ev as MessageEvent).data) });
        } catch {
          /* ignore malformed event */
        }
      });
    }
    es.onerror = () => {
      es.close();
      this.opts.onState("closed");
      if (this.stopped) return;
      this.timer = setTimeout(() => this.connect(), this.retryMs);
      this.retryMs = Math.min(this.retryMs * 2, 30_000);
    };
  }
}
