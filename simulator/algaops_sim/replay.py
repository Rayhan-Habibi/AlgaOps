"""Scenario -> received-message stream, fixture files, and live re-basing."""

from __future__ import annotations

import json
import random
import time
from pathlib import Path

from .faults import PublishEvent, apply_sensor_faults, build_events
from .scenario import Scenario, load_scenario
from .signal_model import WindowSample, generate

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "fixtures"


def generate_stream(sc: Scenario, *, live: bool = False, topic_prefix: str = "algaops/v1",
                    now_ms: int | None = None) -> tuple[list[PublishEvent], list[WindowSample]]:
    """Deterministic event stream.  live=True re-bases time to now and draws a
    fresh boot id (a new run of a real node), so freshness checks see real time."""
    if live:
        now = now_ms if now_ms is not None else time.time_ns() // 1_000_000
        sc = Scenario(**{**sc.__dict__, "start_ms": now - sc.window_ms,
                         "boot_id": random.SystemRandom().getrandbits(32)})
    windows = generate(sc)
    apply_sensor_faults(sc, windows)
    events = build_events(sc, windows, topic_prefix)
    return events, windows


def write_fixture(sc: Scenario, path: Path | None = None) -> Path:
    events, _ = generate_stream(sc)
    path = path or FIXTURE_DIR / f"{sc.name}.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    header = {"fixture": "algaops.publish_stream.v1", "scenario": sc.name,
              "origin_ms": sc.start_ms, "note": "DEMO/SYNTHETIC publish stream; "
              "ground-truth labels are intentionally not stored here"}
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(header, separators=(",", ":")) + "\n")
        for e in events:
            fh.write(json.dumps(e.to_fixture(sc.start_ms), separators=(",", ":")) + "\n")
    return path


def read_fixture(path: Path) -> tuple[dict, list[dict]]:
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    header = json.loads(lines[0])
    if header.get("fixture") != "algaops.publish_stream.v1":
        raise ValueError(f"{path}: not an AlgaOps publish-stream fixture")
    return header, [json.loads(ln) for ln in lines[1:] if ln.strip()]


def rebase_fixture(path: Path, now_ms: int | None = None) -> list[tuple[int, str, bytes, bool]]:
    """Shift a fixture to the current time: (t_abs_ms, topic, payload, retain).
    Timestamps inside valid telemetry payloads move by the same offset."""
    header, items = read_fixture(path)
    now = now_ms if now_ms is not None else time.time_ns() // 1_000_000
    shift = now - int(header["origin_ms"])
    out = []
    for it in items:
        raw = it["payload"]
        try:
            doc = json.loads(raw)
            if isinstance(doc, dict) and "t_end_ms" in doc and isinstance(doc["t_end_ms"], int):
                doc["t_end_ms"] += shift
                if isinstance(doc.get("t_start_ms"), int):
                    doc["t_start_ms"] += shift
                raw = json.dumps(doc, separators=(",", ":"))
        except ValueError:
            pass   # malformed payloads are replayed byte for byte
        out.append((int(header["origin_ms"]) + int(it["t_ms"]) + shift, it["topic"],
                    raw.encode("utf-8"), bool(it.get("retain"))))
    return out


def load_and_generate(name: str, **kw) -> tuple[Scenario, list[PublishEvent],
                                                list[WindowSample]]:
    sc = load_scenario(name)
    events, windows = generate_stream(sc, **kw)
    return sc, events, windows
