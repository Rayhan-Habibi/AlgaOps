import { describe, expect, it } from "vitest";
import { OUTCOME_LABEL, flagText, fmtNumber, fmtPercent, fmtUnit, outcomeClass, shortHash } from "../../src/util/format";
import { fmtAgo, fmtDuration } from "../../src/util/time";

describe("format", () => {
  it("numbers and missing values", () => {
    expect(fmtNumber(0.123456, 3)).toBe("0.123");
    expect(fmtNumber(null)).toBe("—");
    expect(fmtNumber(Number.NaN)).toBe("—");
    expect(fmtUnit(0.42, "g", 2)).toBe("0.42 g");
    expect(fmtUnit(undefined, "g")).toBe("—");
    expect(fmtPercent(0.8766, 1)).toBe("87.7%");
    expect(fmtPercent(null)).toBe("—");
  });

  it("outcome classes and labels cover every outcome", () => {
    expect(outcomeClass("NORMAL")).toBe("ok");
    expect(outcomeClass("CHECK_SENSOR")).toBe("sensor");
    expect(outcomeClass("CHECK_EQUIPMENT")).toBe("equip");
    expect(outcomeClass("NOT_EVALUATED")).toBe("neutral");
    expect(Object.keys(OUTCOME_LABEL)).toHaveLength(4);
  });

  it("flag descriptions", () => {
    expect(flagText("VIB_STUCK")).toContain("stuck");
    expect(flagText("SOMETHING_NEW")).toBe("something new");
  });

  it("hashes and durations", () => {
    expect(shortHash("abcdef0123456789", 6)).toBe("abcdef…");
    expect(shortHash(null)).toBe("—");
    expect(fmtDuration(65_000)).toBe("1 min 5 s");
    expect(fmtDuration(3 * 3600e3)).toBe("3 h 0 min");
    expect(fmtAgo(1000, 1500)).toBe("just now");
    expect(fmtAgo(null)).toBe("never");
  });
});
