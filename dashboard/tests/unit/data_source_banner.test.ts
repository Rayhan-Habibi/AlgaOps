import { describe, expect, it } from "vitest";
import { dataSourceBanner } from "../../src/components/data_source_banner";

describe("data source banner", () => {
  it("labels synthetic data unmistakably", () => {
    const el = dataSourceBanner("DEMO / SYNTHETIC", { simulator: 2 });
    expect(el.className).toContain("demo");
    expect(el.textContent).toContain("DEMO / SYNTHETIC DATA");
    expect(el.textContent).toContain("not measurements");
    expect(el.textContent).toContain("simulator: 2");
  });

  it("labels hardware data as live but not validated", () => {
    const el = dataSourceBanner("LIVE HARDWARE", { hardware: 1 });
    expect(el.className).toContain("live");
    expect(el.textContent).toContain("LIVE HARDWARE");
    expect(el.textContent).toContain("not validated");
  });

  it("warns about mixed sources and handles missing status", () => {
    expect(dataSourceBanner("MIXED: DEMO/SYNTHETIC + LIVE HARDWARE").className).toContain("mixed");
    expect(dataSourceBanner(null).textContent).toContain("NO DATA");
  });

  it("never interprets text as markup", () => {
    const el = dataSourceBanner("LIVE HARDWARE", { "<img src=x onerror=alert(1)>": 1 });
    expect(el.querySelector("img")).toBeNull();
  });
});
