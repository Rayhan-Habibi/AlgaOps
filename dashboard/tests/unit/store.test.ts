import { describe, expect, it } from "vitest";
import { Store } from "../../src/state/store";
import type { NodeSummary } from "../../src/api/types";

const node = (id: string) => ({ node_id: id, data_source: "simulator" } as unknown as NodeSummary);

describe("store", () => {
  it("notifies subscribers and auto-selects the first node", () => {
    const s = new Store();
    const seen: string[] = [];
    const off = s.subscribe((st) => seen.push(st.selectedNode ?? "none"));
    s.set({ nodes: [node("a"), node("b")] });
    expect(s.get().selectedNode).toBe("a");
    s.set({ selectedNode: "b" });
    expect(s.selectedNode()?.node_id).toBe("b");
    off();
    s.set({ apiOk: true });
    expect(seen).toEqual(["a", "b"]);
  });

  it("keeps a bounded, newest-first event list", () => {
    const s = new Store();
    for (let i = 0; i < 10; i++) s.pushEvent("decision", { i }, 5);
    const ev = s.get().liveEvents;
    expect(ev).toHaveLength(5);
    expect(ev[0].data["i"]).toBe(9);
  });

  it("falls back when the selected node disappears", () => {
    const s = new Store({ selectedNode: "gone" });
    s.set({ nodes: [node("x")] });
    expect(s.selectedNode()?.node_id).toBe("x");
  });
});
