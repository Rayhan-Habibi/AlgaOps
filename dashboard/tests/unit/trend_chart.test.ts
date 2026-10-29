import { describe, expect, it } from "vitest";
import { niceTicks, trendChart } from "../../src/components/trend_chart";
import type { Trend } from "../../src/api/types";

const base: Trend = {
  node_id: "n", metric: "vib_rms_g", since_ms: 0, until_ms: 10_000, bucket_ms: 1000,
  points: [], modes: [], markers: [],
};

describe("trend chart", () => {
  it("renders an explicit empty state", () => {
    const svg = trendChart(base);
    expect(svg.textContent).toContain("no data");
    expect(svg.querySelector("path.line")).toBeNull();
  });

  it("draws band, mean line, mode bands and outcome markers", () => {
    const t: Trend = {
      ...base,
      points: [0, 1, 2, 3].map((i) => ({ t: i * 2500, min: 0.4 + i * 0.01, max: 0.45 + i * 0.01,
                                         mean: 0.42 + i * 0.01, n: 3 })),
      modes: [{ mode: "STARTUP", since: 0, until: 2500 }, { mode: "STEADY", since: 2500, until: 10_000 }],
      markers: [{ outcome: "CHECK_SENSOR", since: 5000, until: 6000 },
                { outcome: "CHECK_EQUIPMENT", since: 8000, until: 9000 }],
    };
    const svg = trendChart(t, { unit: "g" });
    expect(svg.querySelector("path.line")?.getAttribute("d")).toMatch(/^M[\d.]+,[\d.]+L/);
    expect(svg.querySelector("polygon.band")).not.toBeNull();
    expect(svg.querySelectorAll(".mode-bands rect")).toHaveLength(2);
    expect(svg.querySelector(".mode-steady")).not.toBeNull();
    expect(svg.querySelector(".mk-sensor")).not.toBeNull();
    expect(svg.querySelector(".mk-equip")).not.toBeNull();
  });

  it("chooses readable ticks, including flat series", () => {
    expect(niceTicks(0, 1, 5)).toEqual([0, 0.2, 0.4, 0.6, 0.8, 1]);
    expect(niceTicks(5, 5).length).toBeGreaterThan(1);
    expect(niceTicks(Number.NaN, 1)).toEqual([]);
  });
});
