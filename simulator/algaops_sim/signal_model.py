"""Deterministic window-level signal model of a pump/blower test rig.

The node transmits window summaries, so the model produces summary features
directly (AC RMS per axis, peak-to-peak, DS18B20-quantised casing
temperature, CT RMS current) with physically consistent relations:
p2p >= rms, |vector| = sqrt(sum axis^2), first-order casing thermal response
around a slowly drifting ambient.  Only `random.Random(seed)` is used, so the
same scenario always yields the same windows on every platform.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field

from .scenario import Scenario

DS18B20_STEP_C = 0.0625


@dataclass
class Label:
    truth: str = "normal"            # normal | sensor_fault | transport_fault | equipment_fault
    fault: str | None = None
    expected: tuple[str, ...] = ("NORMAL", "NOT_EVALUATED")
    mode: str = "OFF"                # true operating mode of the asset

    def as_dict(self) -> dict:
        return {"truth": self.truth, "fault": self.fault, "expected": list(self.expected),
                "mode": self.mode}


@dataclass
class WindowSample:
    index: int
    boot_id: int
    seq: int
    t_start_ms: int
    t_end_ms: int
    window_ms: int
    vib_axes: list[float]
    vib_p2p: list[float]
    temp_c: float | None
    current_a: float | None
    samples_expected: int
    samples_valid: int
    self_test: str = "pass"
    faults: list[str] = field(default_factory=list)
    time_synced: bool = True
    null_fields: set[str] = field(default_factory=set)
    label: Label = field(default_factory=Label)

    @property
    def vib_rms(self) -> float:
        return math.sqrt(sum(a * a for a in self.vib_axes))


def quantize(t: float) -> float:
    return round(t / DS18B20_STEP_C) * DS18B20_STEP_C


def generate(sc: Scenario) -> list[WindowSample]:
    """Normal behaviour + equipment anomalies.  Faults are applied in faults.py."""
    rng = random.Random(sc.seed)
    a = sc.asset
    boot_id = sc.boot_id if sc.boot_id is not None else (0x5EED0000 | (sc.seed & 0xFFFF))
    dt = sc.window_ms / 1000.0
    w_norm = math.sqrt(sum(x * x for x in a.axis_weights))
    weights = [x / w_norm for x in a.axis_weights]
    casing = a.ambient_c
    running_prev = False
    on_since = 0.0
    out: list[WindowSample] = []
    for i in range(sc.duration_windows):
        t_end = sc.start_ms + (i + 1) * sc.window_ms
        t_rel = (i + 1) * dt
        running = sc.running_at(i)
        just_stopped = running_prev and not running
        if running and not running_prev:
            on_since = t_rel - dt
        running_prev = running
        age = t_rel - on_since if running else 0.0
        if not running:
            mode = "OFF"
        elif age * 1000.0 < 20000.0:   # policy.mode.startup_ms (truth uses the same 20 s)
            mode = "STARTUP"
        else:
            mode = "STEADY"

        # --- vibration (AC RMS of the vector) ---------------------------------
        if running:
            start_boost = max(0.0, (a.startup_s - age) / a.startup_s) if a.startup_s > 0 else 0.0
            base = a.run_vib_g + (a.startup_vib_g - a.run_vib_g) * start_boost
            vib = max(0.0, rng.gauss(base, a.run_vib_sd))
        else:
            vib = max(0.0005, rng.gauss(a.off_vib_g, a.off_vib_sd))
            if just_stopped and a.coastdown_tau_s > 0:
                # impeller coast-down: RMS of an exponentially decaying vibration
                frac = (1 - math.exp(-2 * dt / a.coastdown_tau_s)) * a.coastdown_tau_s / (2 * dt)
                vib = max(vib, a.run_vib_g * math.sqrt(frac))
        # --- current --------------------------------------------------------------
        if running:
            inrush = max(0.0, (2.0 - age) / 2.0) * (a.inrush_a - a.run_current_a)
            cur = max(0.0, rng.gauss(a.run_current_a + inrush, a.current_sd))
        else:
            cur = max(0.0, rng.gauss(a.off_current_a, 0.005))
        # --- casing temperature (first-order towards ambient + rise) ---------------
        ambient = a.ambient_c + a.ambient_swing_c * math.sin(2 * math.pi * t_rel /
                                                             a.ambient_period_s)
        target = ambient + (a.temp_rise_c if running else 0.0)
        casing += (target - casing) * (1.0 - math.exp(-dt / a.temp_tau_s))

        extra_temp = 0.0
        vib_factor = 1.0
        cur_factor = 1.0
        eq_fault = None
        for f in sc.equipment_faults:
            if not f.active(i):
                continue
            ramp = max(1, int(f.params.get("ramp_windows", 1)))
            k = min(1.0, (i - f.start + 1) / ramp)
            if f.type == "vibration_increase":
                vib_factor *= 1.0 + (float(f.params.get("factor", 3.0)) - 1.0) * k
            elif f.type == "thermal_rise":
                extra_temp += float(f.params.get("delta_c", 20.0)) * k
            elif f.type == "current_increase":
                cur_factor *= 1.0 + (float(f.params.get("factor", 1.5)) - 1.0) * k
            eq_fault = f.type
        vib *= vib_factor
        cur *= cur_factor
        temp = quantize(casing + extra_temp + rng.gauss(0.0, a.temp_noise_c))

        axes = [round(vib * wgt * rng.uniform(0.97, 1.03), 5) for wgt in weights]
        p2p = [round(ax * rng.uniform(a.crest_min, a.crest_max), 5) for ax in axes]
        label = Label(mode=mode)
        if eq_fault and running:
            label = Label("equipment_fault", eq_fault, ("CHECK_EQUIPMENT",), mode)
        out.append(WindowSample(
            index=i, boot_id=boot_id, seq=i + 1, t_start_ms=t_end - sc.window_ms, t_end_ms=t_end,
            window_ms=sc.window_ms, vib_axes=axes, vib_p2p=p2p, temp_c=temp,
            current_a=round(cur, 3) if sc.current_sensor else None,
            samples_expected=sc.samples_per_window, samples_valid=sc.samples_per_window,
            label=label))
    return out
