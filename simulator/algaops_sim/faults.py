"""Fault injection: sensor faults (window content) and network/transport
faults (delivery).  Every injected fault also sets the ground-truth label of
the affected message; labels travel next to the payload, never inside it.
"""

from __future__ import annotations

import copy
import json
import random
from dataclasses import dataclass

from .payload import build_payload, status_payload
from .scenario import Scenario
from .signal_model import Label, WindowSample, quantize

SENSOR_EXPECT = ("CHECK_SENSOR",)


@dataclass
class PublishEvent:
    t_ms: int                       # time the gateway receives the message (offline model)
    topic: str
    payload: bytes
    kind: str                       # telemetry | status
    label: Label | None = None
    window_index: int | None = None
    redelivery: bool = False        # exact MQTT redelivery (expected to be dropped)

    def to_fixture(self, origin_ms: int) -> dict:
        return {"t_ms": self.t_ms - origin_ms, "topic": self.topic,
                "payload": self.payload.decode("utf-8", errors="replace"), "qos": 1,
                "retain": self.kind == "status"}


def _sensor(w: WindowSample, fault: str) -> None:
    w.label = Label("sensor_fault", fault, SENSOR_EXPECT, w.label.mode)


def apply_sensor_faults(sc: Scenario, windows: list[WindowSample]) -> None:
    rng = random.Random(sc.seed + 7919)
    frozen_value: dict[int, WindowSample] = {}
    for f in sc.sensor_faults:
        for w in windows:
            if not f.active(w.index):
                continue
            t = f.type
            if t == "stuck_vibration":
                if f.params.get("mode", "frozen") == "frozen":
                    w.vib_axes = [0.0, 0.0, 0.0]
                    w.vib_p2p = [0.0, 0.0, 0.0]
                else:   # firmware repeats the last good window's features
                    src = frozen_value.setdefault(id(f), copy.deepcopy(windows[max(0, f.start - 1)]))
                    w.vib_axes = list(src.vib_axes)
                    w.vib_p2p = list(src.vib_p2p)
            elif t == "stuck_temperature":
                src = frozen_value.setdefault(id(f), copy.deepcopy(windows[max(0, f.start - 1)]))
                w.temp_c = src.temp_c
            elif t == "out_of_range":
                if f.params.get("field", "vib") == "vib":
                    v = float(f.params.get("value", 60.0))
                    w.vib_axes = [round(v / 3 ** 0.5, 5)] * 3
                    w.vib_p2p = [round(v * 3.5, 5)] * 3
                else:
                    w.temp_c = float(f.params.get("value", 150.0))
            elif t == "non_finite":
                w.null_fields.add(str(f.params.get("field", "vib")))
            elif t == "self_test_failure":
                w.self_test = "fail"
            elif t == "node_fault":
                w.faults = sorted(set(w.faults) | set(f.params.get("faults", ["TEMP_CRC"])))
            elif t == "sensor_disagreement":   # accelerometer detached, pump still drawing current
                w.vib_axes = [round(rng.uniform(0.005, 0.009), 5) for _ in range(3)]
                w.vib_p2p = [round(a * rng.uniform(4.5, 6.0), 5) for a in w.vib_axes]
            elif t == "clock_unsynced":
                w.time_synced = False
            elif t == "incomplete_window":
                w.samples_valid = int(w.samples_expected * float(f.params.get("valid_ratio", 0.6)))
            elif t == "temp_spike":
                if w.temp_c is not None:
                    w.temp_c = quantize(w.temp_c + float(f.params.get("delta_c", 15.0)))
            elif t == "inconsistent":
                w.vib_p2p[1] = round(w.vib_axes[1] * 0.2, 5)
            _sensor(w, t)
    # sensor_disagreement and repeat-stuck are only detectable after a grace /
    # history period; the label stays sensor_fault (honest window-level recall)


def build_events(sc: Scenario, windows: list[WindowSample], topic_prefix: str = "algaops/v1",
                 base_latency_ms: int = 40) -> list[PublishEvent]:
    """Apply network faults and produce the received-message stream."""
    rng = random.Random(sc.seed + 104729)
    topic = f"{topic_prefix}/nodes/{sc.node_id}/telemetry"
    status_topic = f"{topic_prefix}/nodes/{sc.node_id}/status"
    net = sc.network
    drop: set[int] = set()
    silence: set[int] = set()
    for f in net:
        if f.type == "drop":
            drop |= set(range(f.start, f.start + int(f.params.get("count", 1))))
        elif f.type == "silence":
            silence |= set(range(f.start, f.end if f.end is not None else f.start + 1))

    # reboot: boot id changes and the sequence restarts at 1
    reboots = sorted(f.start for f in net if f.type == "reboot")
    boot = windows[0].boot_id if windows else 0
    seq = 0
    for w in windows:
        if w.index in reboots:
            boot = (boot * 1103515245 + 12345 + w.index) & 0xFFFFFFFF
            seq = 0
        seq += 1
        w.boot_id, w.seq = boot, seq

    events: list[PublishEvent] = [PublishEvent(
        sc.start_ms, status_topic, status_payload(sc, "online", windows[0].boot_id if windows
                                                  else 0), "status")]
    disconnects = [f for f in net if f.type == "disconnect"]
    backlog: list[WindowSample] = []
    for w in windows:
        recv = w.t_end_ms + base_latency_ms + rng.randint(0, 25)
        if w.index in drop or w.index in silence:
            continue
        disc = next((d for d in disconnects if d.start <= w.index < (d.end or d.start + 1)), None)
        if disc is not None:
            cap = int(disc.params.get("buffer_capacity", 300))
            if not backlog:
                events.append(PublishEvent(w.t_start_ms + 10, status_topic,
                                           status_payload(sc, "offline", w.boot_id), "status"))
            backlog.append(w)
            if len(backlog) > cap:
                backlog.pop(0)   # bounded node buffer: oldest window lost
            continue
        if backlog:
            # reconnect: status online, then flush the buffer in order before live data
            t = w.t_start_ms + 200
            events.append(PublishEvent(t, status_topic, status_payload(sc, "online", w.boot_id),
                                       "status"))
            interval = int(disconnects[0].params.get("flush_interval_ms", 20))
            for b in backlog:
                t += interval
                lab = b.label
                if t - b.t_end_ms > 10000 and lab.truth in ("normal", "equipment_fault"):
                    lab = Label("transport_fault", "late_delivery", SENSOR_EXPECT, lab.mode)
                events.append(_telemetry(sc, b, topic, t, lab))
            backlog = []
            recv = max(recv, t + interval)
        delay = next((f for f in net if f.type == "delay" and f.active(w.index)), None)
        if delay is not None:
            recv += int(delay.params.get("delay_ms", 20000))
            if w.label.truth in ("normal", "equipment_fault"):
                w.label = Label("transport_fault", "late_delivery", SENSOR_EXPECT, w.label.mode)
        ts_fault = next((f for f in net if f.type in ("bad_timestamp", "future_timestamp")
                         and f.active(w.index)), None)
        if ts_fault is not None:
            off = int(ts_fault.params.get("offset_ms", -5000 if ts_fault.type == "bad_timestamp"
                                          else 60000))
            w.t_end_ms += off
            w.t_start_ms += off
            w.label = Label("transport_fault", ts_fault.type, SENSOR_EXPECT, w.label.mode)
        events.append(_telemetry(sc, w, topic, recv, w.label))

        for f in net:
            if f.start != w.index:
                continue
            if f.type == "duplicate_exact":
                ev = events[-1]
                events.append(PublishEvent(ev.t_ms + 15, ev.topic, ev.payload, "telemetry",
                                           Label("transport_fault", "duplicate_exact",
                                                 ("DROPPED",), w.label.mode),
                                           w.index, redelivery=True))
            elif f.type == "duplicate_conflict":
                twin = copy.deepcopy(w)
                twin.vib_axes = [round(x * 1.01, 5) for x in twin.vib_axes]
                twin.t_end_ms += 300
                twin.t_start_ms += 300
                events.append(_telemetry(sc, twin, topic, recv + 300,
                                         Label("transport_fault", "duplicate_conflict",
                                               SENSOR_EXPECT, w.label.mode)))
            elif f.type == "malformed":
                events.append(PublishEvent(recv + 200, topic, _malformed(sc, w, f.params),
                                           "telemetry",
                                           Label("transport_fault", "malformed", SENSOR_EXPECT,
                                                 w.label.mode), w.index))

    # reorder: swap delivery of window `at` and `at+1` (the later-arriving one is out of order)
    for f in net:
        if f.type != "reorder":
            continue
        idx = [k for k, e in enumerate(events) if e.window_index in (f.start, f.start + 1)
               and e.kind == "telemetry" and not e.redelivery]
        if len(idx) >= 2:
            a, b = idx[0], idx[1]
            ea, eb = events[a], events[b]
            events[a] = PublishEvent(ea.t_ms, eb.topic, eb.payload, eb.kind, eb.label,
                                     eb.window_index)
            late_label = Label("transport_fault", "out_of_order", SENSOR_EXPECT,
                               ea.label.mode if ea.label else "OFF")
            events[b] = PublishEvent(eb.t_ms, ea.topic, ea.payload, ea.kind, late_label,
                                     ea.window_index)
    events.sort(key=lambda e: e.t_ms)
    return events


def _telemetry(sc: Scenario, w: WindowSample, topic: str, recv: int, label: Label
               ) -> PublishEvent:
    return PublishEvent(recv, topic, build_payload(sc, w), "telemetry", label, w.index)


def _malformed(sc: Scenario, w: WindowSample, params: dict) -> bytes:
    kind = params.get("kind", "invalid_json")
    doc = json.loads(build_payload(sc, w))
    if kind == "invalid_json":
        return build_payload(sc, w)[:-7] + b"#broken"
    if kind == "unknown_field":
        doc["label"] = "equipment_fault"   # a leaked label must be rejected, never used
    elif kind == "wrong_schema":
        doc["schema"] = "algaops.telemetry_window.v0"
    elif kind == "missing_field":
        doc.pop("temp_c")
    elif kind == "wrong_type":
        doc["seq"] = "seventeen"
    return json.dumps(doc, separators=(",", ":")).encode()
