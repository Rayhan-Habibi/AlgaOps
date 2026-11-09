import { beforeEach, describe, expect, it } from "vitest";
import { eventLog, type LogEntry } from "../../src/components/event_log";

describe("event_log component & DOM latency instrumentation", () => {
  beforeEach(() => {
    // Clear global window measurement property before each test
    delete (window as unknown as Record<string, unknown>).__algaops_last_dom_latency_ms;
  });

  it("renders decision events and sets data-dom-latency attributes", () => {
    const receivedMs = Date.now() - 45; // 45 ms in the past
    const entry: LogEntry = {
      type: "decision",
      at: Date.now(),
      data: {
        node_id: "node-pbr-01",
        outcome: "CHECK_EQUIPMENT",
        reason: "vibration anomaly detected",
        received_ms: receivedMs,
      },
    };

    const root = eventLog([entry]);
    expect(root.tagName).toBe("UL");
    expect(root.className).toContain("event-log");

    const li = root.querySelector("li");
    expect(li).not.toBeNull();
    expect(li!.className).toContain("equip");
    expect(li!.textContent).toContain("node-pbr-01");
    expect(li!.textContent).toContain("vibration anomaly detected");

    // Verify DOM latency attributes
    expect(li!.getAttribute("data-received-ms")).toBe(String(receivedMs));
    expect(li!.hasAttribute("data-rendered-ms")).toBe(true);
    expect(li!.hasAttribute("data-dom-latency-ms")).toBe(true);

    const domLatency = Number(li!.getAttribute("data-dom-latency-ms"));
    expect(domLatency).toBeGreaterThanOrEqual(40);
    expect(domLatency).toBeLessThan(5000);

    // Verify window global hook for external/e2e latency auditing
    expect((window as unknown as Record<string, unknown>).__algaops_last_dom_latency_ms).toBe(domLatency);
  });

  it("handles decision with missing received_ms gracefully", () => {
    const entry: LogEntry = {
      type: "decision",
      at: Date.now(),
      data: {
        node_id: "node-pbr-02",
        outcome: "CHECK_SENSOR",
        reason: "sensor out of range",
      },
    };

    const root = eventLog([entry]);
    const li = root.querySelector("li");
    expect(li).not.toBeNull();
    expect(li!.getAttribute("data-received-ms")).toBeNull();
    expect(li!.getAttribute("data-dom-latency-ms")).toBeNull();
    expect(li!.hasAttribute("data-rendered-ms")).toBe(true);
  });

  it("filters out NORMAL decisions without alert events", () => {
    const normalEntry: LogEntry = {
      type: "decision",
      at: Date.now(),
      data: {
        node_id: "node-pbr-01",
        outcome: "NORMAL",
        alert_event: "NONE",
      },
    };

    const root = eventLog([normalEntry]);
    expect(root.tagName).toBe("P");
    expect(root.textContent).toContain("No warnings or events");
  });

  it("renders non-decision event types correctly", () => {
    const entries: LogEntry[] = [
      {
        type: "alert",
        at: Date.now(),
        data: { node_id: "node-1", event: "OPENED", kind: "CHECK_EQUIPMENT" },
      },
      {
        type: "node",
        at: Date.now(),
        data: { node_id: "node-1", state: "online" },
      },
      {
        type: "packet",
        at: Date.now(),
        data: { node_id: "node-1", status: "rejected", reason: "invalid signature" },
      },
      {
        type: "baseline",
        at: Date.now(),
        data: { node_id: "node-1", status: "STEADY captured" },
      },
    ];

    const root = eventLog(entries);
    const items = root.querySelectorAll("li");
    expect(items).toHaveLength(4);
    expect(items[0].className).toContain("alert");
    expect(items[1].className).toContain("info");
    expect(items[2].className).toContain("info");
    expect(items[3].className).toContain("info");
  });
});

