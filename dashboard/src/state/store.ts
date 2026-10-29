// Tiny observable store for state shared between pages.

import type { GatewayStatus, NodeSummary } from "../api/types";
import type { StreamState } from "../api/events";

export interface AppState {
  status: GatewayStatus | null;
  nodes: NodeSummary[];
  selectedNode: string | null;
  apiOk: boolean;
  apiError: string | null;
  lastRefreshMs: number | null;
  stream: StreamState;
  liveEvents: { type: string; data: Record<string, unknown>; at: number }[];
}

type Listener = (s: AppState) => void;

export class Store {
  private state: AppState;
  private listeners = new Set<Listener>();

  constructor(initial?: Partial<AppState>) {
    this.state = {
      status: null,
      nodes: [],
      selectedNode: null,
      apiOk: false,
      apiError: null,
      lastRefreshMs: null,
      stream: "closed",
      liveEvents: [],
      ...initial,
    };
  }

  get(): AppState {
    return this.state;
  }

  set(patch: Partial<AppState>): void {
    this.state = { ...this.state, ...patch };
    if (this.state.selectedNode === null && this.state.nodes.length > 0) {
      this.state.selectedNode = this.state.nodes[0].node_id;
    }
    for (const l of [...this.listeners]) l(this.state);
  }

  subscribe(l: Listener): () => void {
    this.listeners.add(l);
    return () => this.listeners.delete(l);
  }

  pushEvent(type: string, data: Record<string, unknown>, max = 200): void {
    const liveEvents = [{ type, data, at: Date.now() }, ...this.state.liveEvents].slice(0, max);
    this.set({ liveEvents });
  }

  selectedNode(): NodeSummary | null {
    const id = this.state.selectedNode;
    return this.state.nodes.find((n) => n.node_id === id) ?? this.state.nodes[0] ?? null;
  }
}

export const store = new Store();
