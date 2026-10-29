"""Scenario definitions (scenario.v1 YAML) and validation."""

from __future__ import annotations

from dataclasses import dataclass, field, fields
from pathlib import Path
from typing import Any

import yaml

SCHEMA_ID = "algaops.scenario.v1"
SCENARIO_DIR = Path(__file__).resolve().parents[1] / "scenarios"
DEFAULT_START_MS = 1767225600000  # 2026-01-01T00:00:00Z — fixed for reproducible fixtures

SENSOR_FAULTS = {"stuck_vibration", "stuck_temperature", "out_of_range", "non_finite",
                 "self_test_failure", "node_fault", "sensor_disagreement", "clock_unsynced",
                 "incomplete_window", "temp_spike", "inconsistent"}
EQUIPMENT_FAULTS = {"vibration_increase", "thermal_rise", "current_increase"}
NETWORK_FAULTS = {"drop", "duplicate_exact", "duplicate_conflict", "reorder", "bad_timestamp",
                  "future_timestamp", "delay", "disconnect", "malformed", "reboot", "silence"}


class ScenarioError(ValueError):
    pass


@dataclass
class AssetModel:
    """Synthetic pump/blower model (values are illustrative, not measured)."""
    ambient_c: float = 28.0
    ambient_swing_c: float = 0.8          # slow ambient drift amplitude
    ambient_period_s: float = 6 * 3600.0
    off_vib_g: float = 0.012
    off_vib_sd: float = 0.0015
    run_vib_g: float = 0.42
    run_vib_sd: float = 0.006
    startup_vib_g: float = 0.62
    startup_s: float = 8.0
    coastdown_tau_s: float = 0.7
    off_current_a: float = 0.02
    run_current_a: float = 1.80
    current_sd: float = 0.015
    inrush_a: float = 2.9
    temp_rise_c: float = 6.0
    temp_tau_s: float = 900.0
    temp_noise_c: float = 0.02
    axis_weights: tuple[float, float, float] = (0.55, 0.45, 0.70)
    crest_min: float = 4.5
    crest_max: float = 6.0


@dataclass
class FaultSpec:
    type: str
    start: int = 0
    end: int | None = None
    params: dict[str, Any] = field(default_factory=dict)

    def active(self, i: int) -> bool:
        end = self.end if self.end is not None else self.start + 1
        return self.start <= i < end


@dataclass
class Scenario:
    name: str
    description: str
    node_id: str = "sim-pump-01"
    seed: int = 1
    boot_id: int | None = None
    start_ms: int = DEFAULT_START_MS
    window_ms: int = 1280
    samples_per_window: int = 1024
    sample_rate_hz: float = 800.0
    duration_windows: int = 200
    current_sensor: bool = True
    asset: AssetModel = field(default_factory=AssetModel)
    schedule: list[tuple[int, bool]] = field(default_factory=lambda: [(0, True)])
    baseline: str | None = "baseline_modes"
    sensor_faults: list[FaultSpec] = field(default_factory=list)
    equipment_faults: list[FaultSpec] = field(default_factory=list)
    network: list[FaultSpec] = field(default_factory=list)
    source_path: str | None = None

    def running_at(self, i: int) -> bool:
        state = False
        for at, on in self.schedule:
            if at <= i:
                state = on
        return state


def _fault(d: dict[str, Any], allowed: set[str], where: str) -> FaultSpec:
    if not isinstance(d, dict) or "type" not in d:
        raise ScenarioError(f"{where}: each entry needs a 'type'")
    t = d["type"]
    if t not in allowed:
        raise ScenarioError(f"{where}: unknown type {t!r} (allowed: {sorted(allowed)})")
    params = {k: v for k, v in d.items() if k not in ("type", "start", "end", "at")}
    start = int(d.get("start", d.get("at", 0)))
    end = d.get("end")
    return FaultSpec(t, start, int(end) if end is not None else None, params)


def _running_state(v: Any) -> bool:
    """YAML 1.1 reads bare on/off as booleans; accept both spellings."""
    if isinstance(v, bool):
        return v
    s = str(v).strip().lower()
    if s in ("on", "true", "run", "running"):
        return True
    if s in ("off", "false", "stop", "stopped"):
        return False
    raise ScenarioError(f"schedule state must be on/off, got {v!r}")


def from_dict(doc: dict[str, Any], source: str | None = None) -> Scenario:
    if doc.get("schema") != SCHEMA_ID:
        raise ScenarioError(f"{source}: schema must be {SCHEMA_ID}")
    known = {f.name for f in fields(Scenario)} | {"schema"}
    unknown = set(doc) - known
    if unknown:
        raise ScenarioError(f"{source}: unknown key(s) {sorted(unknown)}")
    asset_over = doc.get("asset") or {}
    asset_fields = {f.name for f in fields(AssetModel)}
    if set(asset_over) - asset_fields:
        raise ScenarioError(f"{source}: unknown asset key(s) {sorted(set(asset_over) - asset_fields)}")
    if "axis_weights" in asset_over:
        asset_over = dict(asset_over, axis_weights=tuple(asset_over["axis_weights"]))
    sched = [(int(s["at"]), _running_state(s["state"])) for s in doc.get("schedule", [])] \
        or [(0, True)]
    sc = Scenario(
        name=str(doc["name"]), description=str(doc.get("description", "")),
        node_id=str(doc.get("node_id", "sim-pump-01")), seed=int(doc.get("seed", 1)),
        boot_id=int(doc["boot_id"]) if doc.get("boot_id") is not None else None,
        start_ms=int(doc.get("start_ms", DEFAULT_START_MS)),
        window_ms=int(doc.get("window_ms", 1280)),
        samples_per_window=int(doc.get("samples_per_window", 1024)),
        sample_rate_hz=float(doc.get("sample_rate_hz", 800.0)),
        duration_windows=int(doc.get("duration_windows", 200)),
        current_sensor=bool(doc.get("current_sensor", True)),
        asset=AssetModel(**asset_over), schedule=sorted(sched),
        baseline=doc.get("baseline", "baseline_modes"),
        sensor_faults=[_fault(f, SENSOR_FAULTS, "sensor_faults") for f in
                       doc.get("sensor_faults", [])],
        equipment_faults=[_fault(f, EQUIPMENT_FAULTS, "equipment_faults") for f in
                          doc.get("equipment_faults", [])],
        network=[_fault(f, NETWORK_FAULTS, "network") for f in doc.get("network", [])],
        source_path=source)
    if sc.duration_windows < 1 or sc.duration_windows > 200000:
        raise ScenarioError(f"{source}: duration_windows out of range")
    if sc.window_ms < 100:
        raise ScenarioError(f"{source}: window_ms too small")
    return sc


def load_scenario(name_or_path: str | Path, directory: Path = SCENARIO_DIR) -> Scenario:
    p = Path(name_or_path)
    if not p.suffix:
        p = directory / f"{name_or_path}.yaml"
    with open(p, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh)
    return from_dict(doc, str(p))


def list_scenarios(directory: Path = SCENARIO_DIR) -> list[Scenario]:
    return [load_scenario(p) for p in sorted(directory.glob("*.yaml"))]
