"""Retention: archive, then prune old history without breaking audit verification.

With `[storage] retention_days = N` (N > 0) the gateway removes windows,
decisions, gaps and ingest-log rows older than N days, once a day.  Nothing is
removed before it is in an archive bundle:

1. plan     in a read-only snapshot, select what is older than the cutoff
            (gateway clock - N days); nothing to do -> stop
2. archive  force a checkpoint, write an ordinary evidence bundle scoped to
            everything before the cutoff (export/evidence_package.py, with the
            complete chain) and fsync it.  If the gateway-side verification of
            that snapshot is not OK, nothing is pruned: pruning must never
            remove rows that verification complains about.
3. prune    on the pipeline worker: the transaction that appends the
            `archive.created` event (cutoff, archived_through_seq, bundle file
            name, Ascon-Hash256 digest and chain head) also deletes the first
            batch; the rest follows in batches of BATCH rows, one transaction
            each, and a checkpoint anchors the event in the external file.

Batches instead of one transaction: one transaction for one node-day (67,500
windows) stalled the worker for ~17 s on a desktop PC, longer than the core's
15 s stream timeout, while 1,000-row transactions took < 0.25 s each (1.4 s in
total).  The committed archive.created event covers every batch, so
verification holds after each of them (audit/verify.py counts a missing row as
archived when its event has seq <= archived_through_seq and ts_ms < cutoff_ms).
A run interrupted by a stop leaves rows that the next run archives again.

Every archive holds the complete chain, which is never pruned.  Bundles are
streamed and verification merges the chain with the tables, so the memory an
archive needs does not grow with the chain or the rows (about 8 MB of Python
memory for a node-day, measured on a desktop PC) and the run needs no memory
check; what grows with the chain is each archive's size and the time to write
and verify it.

Never deleted: audit events, alerts (open or closed), baselines, feedback,
checkpoints and nodes; decisions an alert was opened, changed or closed with
(and their windows); the newest window and decision and everything after them,
and the newest gap and ingest-log row, so that row ids are never reused.

Read-only towards the plant like the rest of the gateway: this only deletes
gateway history that is already in an archive bundle.
"""

from __future__ import annotations

import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any

from .export.evidence_package import ExportScope, build_bundle

if TYPE_CHECKING:
    import sqlite3

    from .pipeline import Pipeline
    from .storage.database import Database

log = logging.getLogger(__name__)

DAY_MS = 86_400_000
FIRST_RUN_DELAY_MS = 10 * 60_000     # after a start: the MQTT backlog is processed first
BATCH = 1000                          # rows per prune transaction (see module docstring)
TABLES = ("windows", "diagnostics", "gaps", "ingest_log")

# Decisions an alert was opened, changed or closed with stay, like the alerts.
_ALERT_DECISIONS = ("SELECT open_diag_id FROM alerts WHERE open_diag_id IS NOT NULL UNION "
                    "SELECT close_diag_id FROM alerts WHERE close_diag_id IS NOT NULL")
# A decision (with its window) is pruned when it, its window and its audit event are older
# than the cutoff; verify.py accepts exactly those as archived.
_PRUNABLE = ("FROM diagnostics d JOIN audit_events e ON e.seq = d.audit_seq "
             "LEFT JOIN windows w ON w.id = d.window_id "
             "WHERE d.id < :keep_from AND d.decided_ms < :cutoff AND e.ts_ms < :cutoff "
             "AND (d.window_id IS NULL OR w.received_ms < :cutoff)")
_TIME_COLUMN = {"gaps": "detected_ms", "ingest_log": "received_ms"}


def cutoff_for(now_ms: int, retention_days: int) -> int:
    return now_ms - retention_days * DAY_MS


def utc(ms: int) -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(ms / 1000))


@dataclass
class PrunePlan:
    """What one run removes, with the id / seq bounds fixed in the planning snapshot."""
    cutoff_ms: int
    events: int = 0             # length of the chain, which every bundle holds
    keep_from: int = 0          # decisions with id >= keep_from stay (newest window/decision)
    diagnostics: int = 0
    windows: int = 0
    kept_for_alerts: int = 0
    diag_hi: int = 0            # highest decision id to prune
    through_seq: int = 0        # highest audit seq that references a row to prune
    gaps: int = 0
    gaps_hi: int = 0
    ingest_log: int = 0
    ingest_log_hi: int = 0

    @property
    def empty(self) -> bool:
        return not (self.diagnostics or self.gaps or self.ingest_log)

    def counts(self) -> dict[str, int]:
        return {t: getattr(self, t) for t in TABLES}

    def summary(self) -> dict[str, Any]:
        return {"cutoff_ms": self.cutoff_ms, "cutoff_utc": utc(self.cutoff_ms),
                "rows": self.counts(), "kept_for_alerts": self.kept_for_alerts,
                "archived_through_seq": self.through_seq, "chain_events": self.events}


def plan_prune(conn: sqlite3.Connection, cutoff_ms: int) -> PrunePlan:
    """Select what a run would archive and prune (read-only; run it in one read
    transaction so that the counts and bounds belong to one snapshot)."""
    plan = PrunePlan(cutoff_ms)
    plan.events = conn.execute("SELECT COALESCE(MAX(seq), 0) FROM audit_events").fetchone()[0]
    newest = conn.execute("SELECT MAX(id) FROM diagnostics").fetchone()[0]
    if newest is not None:
        last_window = conn.execute("SELECT MIN(id) FROM diagnostics WHERE window_id = "
                                   "(SELECT MAX(id) FROM windows)").fetchone()[0]
        plan.keep_from = min(newest, last_window if last_window is not None else newest)
        args = {"keep_from": plan.keep_from, "cutoff": cutoff_ms}
        n, nw, hi, through = conn.execute(
            f"SELECT COUNT(*), COUNT(d.window_id), MAX(d.id), MAX(d.audit_seq) {_PRUNABLE} "
            f"AND d.id NOT IN ({_ALERT_DECISIONS})", args).fetchone()
        plan.diagnostics, plan.windows = n, nw
        plan.diag_hi, plan.through_seq = hi or 0, through or 0
        plan.kept_for_alerts = conn.execute(
            f"SELECT COUNT(*) {_PRUNABLE} AND d.id IN ({_ALERT_DECISIONS})", args).fetchone()[0]
    for table, col in _TIME_COLUMN.items():
        n, hi = conn.execute(f"SELECT COUNT(*), MAX(id) FROM {table} WHERE {col} < ? AND "
                             f"id < (SELECT MAX(id) FROM {table})", (cutoff_ms,)).fetchone()
        setattr(plan, table, n)
        setattr(plan, f"{table}_hi", hi or 0)
    return plan


def plan_snapshot(db: Database, cutoff_ms: int) -> PrunePlan:
    conn = db.connect_readonly()
    try:
        conn.execute("BEGIN")
        try:
            return plan_prune(conn, cutoff_ms)
        finally:
            conn.execute("COMMIT")
    finally:
        conn.close()


def _fsync(path: Path) -> None:
    """The archive must be on disk before anything it holds is deleted."""
    fd = os.open(path, os.O_RDWR | getattr(os, "O_BINARY", 0))
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    if hasattr(os, "O_DIRECTORY"):          # POSIX: the rename of the .part file, too
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


@dataclass
class PruneJob:
    """The prune batches of one run, after its archive has been written."""
    plan: PrunePlan
    retention_days: int
    archive: Path
    manifest: dict[str, Any]
    deleted: dict[str, int] = field(default_factory=lambda: dict.fromkeys(TABLES, 0))
    event_seq: int | None = None
    _after: dict[str, int] = field(default_factory=lambda: dict.fromkeys(_TIME_COLUMN, 0) |
                                   {"diagnostics": 0})

    def event_body(self) -> dict[str, Any]:
        chain = self.manifest["chain"]
        return {"cutoff_ms": self.plan.cutoff_ms, "retention_days": self.retention_days,
                "archived_through_seq": self.plan.through_seq, "file": self.archive.name,
                "bundle_digest": self.manifest["bundle_digest"],
                "bundle_head_seq": chain["head_seq"], "bundle_head_hash": chain["head_hash"],
                "prune": self.plan.counts(), "kept_for_alerts": self.plan.kept_for_alerts}

    def step(self, p: Pipeline) -> bool:
        """One transaction on the writer (pipeline worker); True when nothing is left."""
        ev = None
        try:
            with p.db.transaction() as tx:
                if self.event_seq is None:
                    ev = p.chain.append(tx, "archive.created", p.clock(), None, self.event_body())
                deleted, after, done = self._batch(tx)
        except Exception:
            p.chain.rollback()
            if self.event_seq is None:      # no event names the archive: do not leave it behind
                self.archive.unlink(missing_ok=True)
            raise
        p.chain.commit()
        if ev is not None:
            self.event_seq = ev.seq
        self._after = after
        for table, n in deleted.items():
            self.deleted[table] += n
        return done

    def _batch(self, tx: sqlite3.Connection) -> tuple[dict[str, int], dict[str, int], bool]:
        """Delete up to BATCH rows of the next table: (deleted, cursors, finished)."""
        plan, after = self.plan, dict(self._after)
        if after["diagnostics"] < plan.diag_hi:
            tx.execute("CREATE TEMP TABLE IF NOT EXISTS prune_batch (id INTEGER PRIMARY KEY, "
                       "window_id INTEGER)")
            tx.execute(f"INSERT INTO temp.prune_batch (id, window_id) SELECT d.id, d.window_id "
                       f"{_PRUNABLE} AND d.id > :after AND d.id <= :hi AND "
                       f"d.audit_seq <= :through AND d.id NOT IN ({_ALERT_DECISIONS}) "
                       "ORDER BY d.id LIMIT :n",
                       {"keep_from": plan.keep_from, "cutoff": plan.cutoff_ms,
                        "after": after["diagnostics"], "hi": plan.diag_hi,
                        "through": plan.through_seq, "n": BATCH})
            last = tx.execute("SELECT MAX(id) FROM temp.prune_batch").fetchone()[0]
            if last is not None:
                # decisions first: deleting a window that a decision still references fails
                nd = tx.execute("DELETE FROM diagnostics WHERE id IN "
                                "(SELECT id FROM temp.prune_batch)").rowcount
                nw = tx.execute("DELETE FROM windows WHERE id IN (SELECT window_id FROM "
                                "temp.prune_batch WHERE window_id IS NOT NULL)").rowcount
                tx.execute("DELETE FROM temp.prune_batch")
                after["diagnostics"] = last
                return {"diagnostics": nd, "windows": nw}, after, False
            tx.execute("DROP TABLE temp.prune_batch")
            after["diagnostics"] = plan.diag_hi
        for table, col in _TIME_COLUMN.items():
            hi = getattr(plan, f"{table}_hi")
            if after[table] >= hi:
                continue
            last = tx.execute(f"SELECT MAX(id) FROM (SELECT id FROM {table} WHERE id > ? AND "
                              f"id <= ? AND {col} < ? ORDER BY id LIMIT ?)",
                              (after[table], hi, plan.cutoff_ms, BATCH)).fetchone()[0]
            if last is not None:
                n = tx.execute(f"DELETE FROM {table} WHERE id > ? AND id <= ? AND {col} < ?",
                               (after[table], last, plan.cutoff_ms)).rowcount
                after[table] = last
                return {table: n}, after, False
            after[table] = hi
        return {}, after, True

    def finish(self, p: Pipeline) -> dict[str, Any]:
        p.checkpoint_now()           # anchors archive.created in the external checkpoint file
        res = self.result()
        log.info("retention: rows before %s archived to %s (archive.created seq %s), pruned %s",
                 utc(self.plan.cutoff_ms), self.archive.name, self.event_seq, self.deleted)
        return res

    def result(self) -> dict[str, Any]:
        return {"ok": True, "retention_days": self.retention_days, **self.plan.summary(),
                "file": self.archive.name, "path": str(self.archive),
                "bundle_digest": self.manifest["bundle_digest"],
                "recorded_as_seq": self.event_seq, "pruned": dict(self.deleted)}


def archive(p: Pipeline, plan: PrunePlan, retention_days: int, now: int
            ) -> tuple[PruneJob | None, dict[str, Any]]:
    """Write the archive bundle (reads only); returns the job, or None and the reason.
    The caller has forced a checkpoint, so the bundle holds one at its head."""
    if p.archive_dir is None:
        raise ValueError("retention needs an archive directory (storage.export_dir)")
    stamp = time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(now / 1000))
    path = p.archive_dir / f"algaops-evidence-{p.gateway_id}-archive-{stamp}-{now % 1000:03d}.zip"
    conn = p.db.connect_readonly()
    try:
        manifest = build_bundle(conn, p.hasher, path, gateway_id=p.gateway_id,
                                engine_version=p.lib.version, policy_hash=p.policy.hash,
                                created_ms=now, scope=ExportScope(until_ms=plan.cutoff_ms - 1),
                                checkpoint_file=p.checkpoints.path)
    except BaseException:
        path.with_suffix(path.suffix + ".part").unlink(missing_ok=True)
        raise
    finally:
        conn.close()
    if not manifest["verification"]["ok"]:
        path.unlink()                # the database still holds all of it
        n = manifest["verification"]["issues"]
        log.error("retention: verification found %d issue(s); nothing archived or pruned. "
                  "Run `algaops-gateway verify`.", n)
        return None, {"ok": False, "retention_days": retention_days, **plan.summary(),
                      "error": f"verification found {n} issue(s); nothing was pruned"}
    _fsync(path)
    return PruneJob(plan, retention_days, path, manifest), {}


def run(p: Pipeline, retention_days: int, now: int | None = None) -> dict[str, Any]:
    """The whole procedure, waiting for it (CLI `prune`: no worker thread; never call it
    on the worker, the archive would block ingestion)."""
    now = p.clock() if now is None else now
    plan = plan_snapshot(p.db, cutoff_for(now, retention_days))
    if plan.empty:
        return {"ok": True, "retention_days": retention_days, **plan.summary(), "file": None,
                "pruned": dict.fromkeys(TABLES, 0)}
    p.checkpoint_now()
    job, failure = archive(p, plan, retention_days, now)
    if job is None:
        return failure
    while not p.call(job.step, timeout=600):
        pass
    return p.call(job.finish)


def run_in_background(p: Pipeline, retention_days: int, now: int) -> None:
    """Daily run, started by Pipeline.tick() on the worker after it forced a checkpoint:
    plan and archive on this thread, then the worker prunes one batch per tick."""
    job = None
    try:
        plan = plan_snapshot(p.db, cutoff_for(now, retention_days))
        if not plan.empty:
            job, _ = archive(p, plan, retention_days, now)
    except Exception:
        log.exception("retention: archive failed; nothing was pruned")
        job = None
    p.submit(lambda q: q.start_prune(job))
