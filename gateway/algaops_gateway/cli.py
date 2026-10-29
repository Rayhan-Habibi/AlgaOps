"""algaops-gateway command line."""

from __future__ import annotations

import argparse
import contextlib
import json
import logging
import sys
import time
from pathlib import Path

from .config import ConfigError, load_config
from .logging_setup import setup_logging
from .storage.writer_lock import WriterLockHeld

log = logging.getLogger("algaops_gateway")


def _cfg(args):
    return load_config(args.config)


def cmd_run(args) -> int:
    from .app import GatewayApp
    cfg = _cfg(args)
    setup_logging(cfg.logging.level, cfg.resolve(cfg.logging.file), cfg.logging.json)
    GatewayApp(cfg).serve()
    return 0


def cmd_devbroker(args) -> int:
    """Local development MQTT broker (amqtt). Production uses Mosquitto."""
    import asyncio

    from amqtt.broker import Broker
    setup_logging("INFO")
    config = {"listeners": {"default": {"type": "tcp", "bind": f"{args.host}:{args.port}"}},
              "plugins": {"amqtt.plugins.authentication.AnonymousAuthPlugin":
                          {"allow_anonymous": True}}}

    async def main() -> None:
        broker = Broker(config)
        await broker.start()
        log.warning("DEVELOPMENT broker on %s:%s (anonymous, no TLS). Use Mosquitto with "
                    "deploy/mosquitto/*.conf for deployments.", args.host, args.port)
        try:
            while True:
                await asyncio.sleep(3600)
        finally:
            await broker.shutdown()
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    return 0


def _open(cfg):
    from .core.engine import load_policy
    from .core.libalgaops import load_library
    from .storage.database import Database
    lib = load_library(cfg.resolve(cfg.engine.library_path))
    policy = load_policy(lib, cfg.resolve(cfg.engine.policy_file))
    return lib, policy, Database(cfg.resolve(cfg.storage.db_path))


@contextlib.contextmanager
def _writer(cfg):
    """The gateway's pipeline, opened for a write command.  Opening takes the database's
    writer lock: WriterLockHeld (exit 2) while the gateway or another write command runs."""
    from .app import GatewayApp
    g = GatewayApp(cfg)
    try:
        g.pipeline.open()
        try:
            yield g.pipeline
        finally:
            g.pipeline.close()
    finally:
        g.db.close()


def cmd_verify(args) -> int:
    from .audit.ascon import Hasher
    from .audit.verify import verify_database
    cfg = _cfg(args)
    lib, policy, db = _open(cfg)
    conn = db.connect_readonly()
    conn.execute("BEGIN")
    rep = verify_database(conn, Hasher(lib), cfg.resolve(cfg.storage.checkpoint_file))
    conn.execute("COMMIT")
    print(json.dumps(rep.to_dict(), indent=2))
    return 0 if rep.ok else 3


def cmd_export(args) -> int:
    from .audit.ascon import Hasher
    from .export.evidence_package import ExportScope, build_bundle
    cfg = _cfg(args)
    lib, policy, db = _open(cfg)
    out = Path(args.out) if args.out else cfg.resolve(cfg.storage.export_dir) / \
        f"algaops-evidence-{cfg.gateway_id}-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}.zip"
    conn = db.connect_readonly()
    man = build_bundle(conn, Hasher(lib), out, gateway_id=cfg.gateway_id,
                       engine_version=lib.version, policy_hash=policy.hash,
                       created_ms=time.time_ns() // 1_000_000,
                       scope=ExportScope(args.node, args.since, args.until),
                       checkpoint_file=cfg.resolve(cfg.storage.checkpoint_file))
    print(json.dumps({"file": str(out), "chain": man["chain"], "data_label": man["data_label"],
                      "verification": man["verification"]}, indent=2))
    return 0 if man["verification"]["ok"] else 3


def cmd_prune(args) -> int:
    """Archive, then prune rows older than the retention period (retention.py)."""
    import sqlite3

    from .retention import cutoff_for, plan_prune
    from .storage.database import now_ms
    cfg = _cfg(args)
    days = cfg.storage.retention_days if args.days is None else args.days
    if days < 1:
        print("nothing to prune: [storage] retention_days = 0 keeps everything (set it, or "
              "pass --days N)", file=sys.stderr)
        return 2
    db_path = cfg.resolve(cfg.storage.db_path)
    if not db_path.is_file():
        print(f"error: database {db_path} not found", file=sys.stderr)
        return 2
    if args.dry_run:            # read-only connection: no migration, no audit event, no file
        conn = sqlite3.connect(f"{db_path.resolve().as_uri()}?mode=ro", uri=True,
                               isolation_level=None)
        conn.row_factory = sqlite3.Row
        try:
            conn.execute("BEGIN")
            plan = plan_prune(conn, cutoff_for(now_ms(), days))
            conn.execute("COMMIT")
        finally:
            conn.close()
        print(json.dumps({"dry_run": True, "retention_days": days, **plan.summary()}, indent=2))
        return 0
    setup_logging("WARNING")
    with _writer(cfg) as pipe:
        res = pipe.prune_now(days)
    print(json.dumps(res, indent=2))
    return 0 if res["ok"] else 3


def cmd_evaluate(args) -> int:
    from .analytics.evaluate import FULL, QUICK, evaluate
    cfg = _cfg(args)
    setup_logging("WARNING")
    lib, policy, _db = _open(cfg)
    rep = evaluate(lib, policy, FULL if args.full else QUICK)
    out = Path(args.out) if args.out else cfg.resolve("var/evaluation/latest.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(rep, indent=1), encoding="utf-8")
    o = rep["overall"]
    print(rep["disclaimer"])
    print(f"trust detection   {o['trust_detection']}")
    print(f"equipment         {o['equipment_detection']}")
    for m, c in o["methods_steady"].items():
        print(f"method {m:11s} precision={c['precision']} recall={c['recall']} fpr={c['fpr']}")
    print(f"attribution errors (CHECK_EQUIPMENT on sensor/transport faults): "
          f"{o['attribution_errors']}")
    print(f"report: {out}")
    return 0


def cmd_replay(args) -> int:
    """Offline replay of a publish-stream fixture into a separate database."""
    from .audit.checkpoint import CheckpointWriter
    from .pipeline import Pipeline
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "simulator"))
    from algaops_sim.replay import read_fixture
    cfg = _cfg(args)
    lib, policy, _ = _open(cfg)
    from .storage.database import Database
    db = Database(args.db)
    header, items = read_fixture(Path(args.fixture))
    clock = {"t": int(header["origin_ms"])}
    pipe = Pipeline(lib=lib, policy=policy, db=db, gateway_id=f"{cfg.gateway_id}-replay",
                    checkpoint_writer=CheckpointWriter(Path(args.db).with_suffix(".cp.jsonl")),
                    clock=lambda: clock["t"])
    pipe.open()
    for it in items:
        t = int(header["origin_ms"]) + int(it["t_ms"])
        while clock["t"] + 1000 <= t:
            clock["t"] += 1000
            pipe.tick(clock["t"])
        clock["t"] = t
        pipe.handle_message(it["topic"], it["payload"].encode("utf-8"), t)
    pipe.close()
    print(json.dumps(pipe.stats.snapshot(), indent=2))
    return 0


def cmd_baseline(args) -> int:
    cfg = _cfg(args)
    setup_logging("WARNING")
    if args.action == "list":
        lib, policy, db = _open(cfg)
        for r in db.writer.execute("SELECT id, node_id, mode, status, n, created_ms FROM baselines "
                                   "ORDER BY id"):
            print(dict(r))
        return 0
    with _writer(cfg) as pipe:
        if args.action == "import":
            ids = pipe.import_baseline_csv(args.node, args.csv, activate=args.activate)
            print(json.dumps({"baseline_ids": ids}))
        elif args.action == "activate":
            print(json.dumps(pipe.activate_baseline(int(args.id))))
    return 0


def cmd_policy(args) -> int:
    cfg = _cfg(args)
    lib, policy, _ = _open(cfg)
    print(json.dumps({"file": policy.path, "meta": policy.meta, "hash": policy.hash,
                      "effective": policy.effective}, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(prog="algaops-gateway",
                                description="AlgaOps Assurance edge gateway (read-only)")
    p.add_argument("--config", help="gateway TOML config (default: built-in safe defaults)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("run", help="start MQTT ingest + API").set_defaults(fn=cmd_run)
    b = sub.add_parser("devbroker", help="local development MQTT broker")
    b.add_argument("--host", default="127.0.0.1")
    b.add_argument("--port", type=int, default=1883)
    b.set_defaults(fn=cmd_devbroker)
    sub.add_parser("verify", help="verify the audit chain").set_defaults(fn=cmd_verify)
    e = sub.add_parser("export", help="write an evidence bundle")
    e.add_argument("--out")
    e.add_argument("--node")
    e.add_argument("--since", type=int)
    e.add_argument("--until", type=int)
    e.set_defaults(fn=cmd_export)
    pr = sub.add_parser("prune", help="archive, then prune rows older than the retention period "
                                      "(gateway stopped; --dry-run only counts)")
    pr.add_argument("--dry-run", action="store_true", help="count what would be pruned, change "
                                                           "nothing")
    pr.add_argument("--days", type=int, help="retention in days (default: storage.retention_days)")
    pr.set_defaults(fn=cmd_prune)
    ev = sub.add_parser("evaluate", help="labelled synthetic evaluation")
    ev.add_argument("--full", action="store_true", help="include the 8 h and long scenarios")
    ev.add_argument("--out")
    ev.set_defaults(fn=cmd_evaluate)
    r = sub.add_parser("replay", help="offline replay of a fixture into a separate DB")
    r.add_argument("fixture")
    r.add_argument("--db", required=True)
    r.set_defaults(fn=cmd_replay)
    bl = sub.add_parser("baseline", help="baseline management")
    bl.add_argument("action", choices=["list", "import", "activate"])
    bl.add_argument("--node")
    bl.add_argument("--csv")
    bl.add_argument("--id")
    bl.add_argument("--activate", action="store_true")
    bl.set_defaults(fn=cmd_baseline)
    sub.add_parser("policy", help="show the effective policy and its hash").set_defaults(
        fn=cmd_policy)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except ConfigError as exc:
        print(f"configuration error: {exc}", file=sys.stderr)
        return 2
    except WriterLockHeld as exc:
        hint = ("the gateway can start once that process has finished" if args.cmd == "run" else
                "stop the gateway first, or wait for that command to finish; verify, export, "
                "prune --dry-run and baseline list work while it runs")
        print(f"refused: {exc}\n{hint}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
