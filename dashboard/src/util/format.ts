// Formatting helpers.  All UI text is inserted with textContent, never innerHTML.

import type { Outcome } from "../api/types";

export function fmtNumber(v: number | null | undefined, digits = 3): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "—";
  return v.toFixed(digits);
}

export function fmtPercent(v: number | null | undefined, digits = 0): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return "—";
  return `${(v * 100).toFixed(digits)}%`;
}

export function fmtUnit(v: number | null | undefined, unit: string, digits = 3): string {
  const s = fmtNumber(v, digits);
  return s === "—" ? s : `${s} ${unit}`;
}

export function shortHash(h: string | null | undefined, n = 12): string {
  if (!h) return "—";
  return h.length > n ? `${h.slice(0, n)}…` : h;
}

export const OUTCOME_LABEL: Record<Outcome, string> = {
  NORMAL: "NORMAL",
  CHECK_SENSOR: "CHECK SENSOR",
  CHECK_EQUIPMENT: "CHECK EQUIPMENT",
  NOT_EVALUATED: "NOT EVALUATED",
};

export const OUTCOME_HINT: Record<Outcome, string> = {
  NORMAL: "Trusted data within the configured normal criteria.",
  CHECK_SENSOR: "Data integrity problem: inspect the sensor or the communication path first. "
    + "Equipment health is not judged on this data.",
  CHECK_EQUIPMENT: "Trusted data deviates from the baseline or a fixed limit: inspect the "
    + "pump/blower. This is an inspection request, not a diagnosis.",
  NOT_EVALUATED: "Trusted data, but no criteria could be evaluated (no baseline for this mode, "
    + "or a start/stop transient).",
};

export function outcomeClass(o: string | null | undefined): string {
  switch (o) {
    case "NORMAL": return "ok";
    case "CHECK_SENSOR": return "sensor";
    case "CHECK_EQUIPMENT": return "equip";
    case "NOT_EVALUATED": return "neutral";
    default: return "neutral";
  }
}

const FLAG_TEXT: Record<string, string> = {
  MISSING_PACKET: "packets missing before this window (informational)",
  DUPLICATE_SEQ: "duplicate sequence number",
  OUT_OF_ORDER_SEQ: "out-of-order / superseded session",
  BAD_TIMESTAMP: "timestamp not advancing",
  VIB_OUT_OF_RANGE: "vibration outside plausible range",
  TEMP_OUT_OF_RANGE: "temperature outside plausible range",
  VIB_STUCK: "accelerometer stuck / frozen",
  TEMP_STUCK: "temperature reading stuck",
  NON_FINITE: "missing / non-finite value",
  MALFORMED: "malformed message",
  STALE: "delivered too late",
  FUTURE_TIMESTAMP: "timestamp ahead of gateway clock",
  REBOOT: "node rebooted (informational)",
  INCOMPLETE_WINDOW: "too few valid samples in window",
  SELF_TEST_FAIL: "accelerometer self-test failed",
  SENSOR_FAULT: "node reported a sensor/bus fault",
  CROSS_SENSOR: "current and vibration disagree",
  CLOCK_UNSYNCED: "node clock not synchronised",
  INCONSISTENT: "internally inconsistent window",
  LOW_COMPLETENESS: "too many recent windows lost",
  SPIKE: "implausible temperature jump",
  STREAM_TIMEOUT: "no data received",
};

export function flagText(flag: string): string {
  return FLAG_TEXT[flag] ?? flag.toLowerCase().replace(/_/g, " ");
}

export const INFORMATIONAL_FLAGS = new Set(["MISSING_PACKET", "REBOOT"]);

export function dataSourceLabel(label: string): { text: string; cls: string } {
  if (label.startsWith("MIXED")) return { text: label, cls: "mixed" };
  if (label === "LIVE HARDWARE") return { text: "LIVE HARDWARE", cls: "live" };
  if (label === "NO DATA") return { text: "NO DATA YET", cls: "nodata" };
  return { text: "DEMO / SYNTHETIC DATA", cls: "demo" };
}
