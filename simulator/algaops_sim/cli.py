"""Command line: list / show / generate fixtures / export CSV / labels / publish."""

from __future__ import annotations

import argparse
import dataclasses
import json
import sys
import threading
from pathlib import Path

from .csv_export import export_csv
from .mqtt_publisher import Publisher, offline_status, publish_schedule
from .replay import FIXTURE_DIR, generate_stream, rebase_fixture, write_fixture
from .scenario import list_scenarios, load_scenario

NO_FIXTURE = {"normal_8h_replay", "baseline_modes"}   # large / commissioning-only streams


def cmd_list(args) -> int:
    for sc in list_scenarios():
        print(f"{sc.name:30s} {sc.duration_windows:6d} windows  {sc.description.splitlines()[0]}")
    return 0


def cmd_show(args) -> int:
    sc = load_scenario(args.scenario)
    events, windows = generate_stream(sc)
    tele = [e for e in events if e.kind == "telemetry"]
    counts: dict[str, int] = {}
    for e in tele:
        k = e.label.truth if e.label else "?"
        counts[k] = counts.get(k, 0) + 1
    print(json.dumps({"name": sc.name, "description": sc.description, "windows": len(windows),
                      "messages": len(events), "telemetry": len(tele),
                      "truth_counts": counts}, indent=2))
    return 0


def cmd_generate(args) -> int:
    names = [s.name for s in list_scenarios()] if args.scenario == "all" else [args.scenario]
    out = Path(args.out) if args.out else FIXTURE_DIR
    for n in names:
        if args.scenario == "all" and n in NO_FIXTURE:
            continue
        p = write_fixture(load_scenario(n), out / f"{n}.jsonl")
        print(f"wrote {p}")
    return 0


def cmd_export_csv(args) -> int:
    sc = load_scenario(args.scenario)
    events, _ = generate_stream(sc)
    n = export_csv(events, Path(args.out), sc.name, sc.description)
    print(f"wrote {n} rows to {args.out}")
    return 0


def cmd_labels(args) -> int:
    sc = load_scenario(args.scenario)
    events, _ = generate_stream(sc)
    for e in events:
        if e.kind == "telemetry":
            print(json.dumps({"t_ms": e.t_ms - sc.start_ms, "window": e.window_index,
                              **(e.label.as_dict() if e.label else {})}))
    return 0


def cmd_publish(args) -> int:
    if args.fixture:
        items = rebase_fixture(Path(args.fixture))
        node = json.loads(items[0][2]).get("node_id", "sim") if items else "sim"
    else:
        sc = load_scenario(args.scenario)
        if args.node_id:
            sc = dataclasses.replace(sc, node_id=args.node_id)
        events, _ = generate_stream(sc, live=True)
        items = [(e.t_ms, e.topic, e.payload, e.kind == "status") for e in events]
        node = sc.node_id
    lwt_topic, lwt_payload = offline_status(node)
    stop = threading.Event()
    print(f"publishing {len(items)} messages in real time to {args.host}:{args.port} "
          "(DEMO/SYNTHETIC, Ctrl+C to stop)")
    try:
        with Publisher(args.host, args.port, client_id=f"algaops-sim-{node}",
                       lwt_topic=lwt_topic, lwt_payload=lwt_payload) as pub:
            sent = publish_schedule(items, pub, stop)
            pub.publish(lwt_topic, lwt_payload, retain=True)
    except KeyboardInterrupt:
        stop.set()
        return 130
    print(f"sent {sent} messages")
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="algaops-sim",
                                description="AlgaOps deterministic scenario simulator "
                                            "(DEMO/SYNTHETIC data only)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list").set_defaults(fn=cmd_list)
    s = sub.add_parser("show")
    s.add_argument("scenario")
    s.set_defaults(fn=cmd_show)
    g = sub.add_parser("generate", help="write publish-stream fixtures")
    g.add_argument("scenario", help="scenario name or 'all'")
    g.add_argument("--out")
    g.set_defaults(fn=cmd_generate)
    c = sub.add_parser("export-csv", help="v2 CSV for the C CLI")
    c.add_argument("scenario")
    c.add_argument("--out", required=True)
    c.set_defaults(fn=cmd_export_csv)
    lb = sub.add_parser("labels", help="print ground-truth labels (never published)")
    lb.add_argument("scenario")
    lb.set_defaults(fn=cmd_labels)
    pb = sub.add_parser("publish", help="publish to MQTT in real time")
    src = pb.add_mutually_exclusive_group(required=True)
    src.add_argument("--scenario")
    src.add_argument("--fixture")
    pb.add_argument("--host", default="127.0.0.1")
    pb.add_argument("--port", type=int, default=1883)
    pb.add_argument("--node-id", help="publish a scenario under another node id "
                                     "(several simulated nodes at once)")
    pb.set_defaults(fn=cmd_publish)
    args = p.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
