"""Ingestion pipeline: MQTT message -> validation -> C core -> SQLite + audit.

All writes (windows, decisions, alerts, baselines, feedback, audit events,
checkpoints) happen on one worker thread, so the audit chain has a single
writer and a total order.  Across processes, open() takes the database's
writer lock (storage/writer_lock.py) until close(): a write command run next
to the gateway is refused instead of forking the chain.  Per telemetry message,
one SQLite transaction stores the window, its decision, any alert transition
and the audit events; the MQTT PUBACK is sent only after that commit.

The pipeline contains no diagnostic logic: outcomes, trust, modes, alerts and
stream timeouts all come from the C engine.

With retention_days > 0 the tick also starts the daily archive-then-prune run
(retention.py): the archive is written on a separate thread from a read-only
snapshot, the prune batches run on this worker, one per tick.
"""

from __future__ import annotations

import json
import logging
import queue
import threading
import time
from collections.abc import Callable
from concurrent.futures import Future
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from . import retention
from .api.events import EventBus
from .audit.ascon import Hasher
from .audit.canonical import canonical_json
from .audit.checkpoint import Checkpoint, CheckpointWriter
from .audit.hash_chain import AuditChain
from .core.engine import NodeEngine, Policy, baselines_from_csv
from .core.libalgaops import FEATURES, CoreLibrary
from .diagnostics.baseline_manager import (
    BaselineCapture,
    baseline_to_stats,
    mode_index,
    stats_json,
    stats_to_baseline,
)
from .diagnostics.decision import DecisionRecord
from .diagnostics.feedback import validate_feedback
from .ingest.idempotency import IdempotencyGuard
from .ingest.node_status import parse_status
from .ingest.payload_schema import NormalizedWindow, PayloadError, parse_telemetry
from .ingest.topics import parse_topic
from .storage import repositories as repo
from .storage.database import Database, now_ms
from .storage.writer_lock import WriterLock

log = logging.getLogger(__name__)

_STOP = object()


@dataclass
class PipelineStats:
    received: int = 0
    accepted: int = 0
    duplicate_deliveries: int = 0
    rejected: int = 0
    stream_timeouts: int = 0
    storage_errors: int = 0
    queue_full: int = 0
    last_latency_ms: float | None = None
    latencies_ms: list[float] = field(default_factory=list)

    def note_latency(self, ms: float) -> None:
        self.last_latency_ms = ms
        self.latencies_ms.append(ms)
        if len(self.latencies_ms) > 5000:
            del self.latencies_ms[:1000]

    def snapshot(self) -> dict[str, Any]:
        lat = sorted(self.latencies_ms)

        def pct(p: float) -> float | None:
            return round(lat[min(len(lat) - 1, int(p * len(lat)))], 3) if lat else None
        return {"received": self.received, "accepted": self.accepted,
                "duplicate_deliveries": self.duplicate_deliveries, "rejected": self.rejected,
                "stream_timeouts": self.stream_timeouts, "storage_errors": self.storage_errors,
                "queue_full": self.queue_full,
                "latency_ms": {"last": self.last_latency_ms, "p50": pct(0.5), "p95": pct(0.95),
                               "max": lat[-1] if lat else None, "samples": len(lat)}}


class Pipeline:
    def __init__(self, *, lib: CoreLibrary, policy: Policy, db: Database, gateway_id: str,
                 topic_prefix: str = "algaops/v1", events: EventBus | None = None,
                 checkpoint_writer: CheckpointWriter | None = None,
                 checkpoint_every_events: int = 500, checkpoint_every_s: float = 300.0,
                 tick_interval_s: float = 1.0, warm_start_windows: int = 1200,
                 retention_days: int = 0, archive_dir: str | Path | None = None,
                 clock: Callable[[], int] = now_ms) -> None:
        if retention_days and archive_dir is None:
            raise ValueError("retention_days > 0 needs an archive_dir")
        self.lib = lib
        self.policy = policy
        self.db = db
        self.gateway_id = gateway_id
        self.topic_prefix = topic_prefix
        self.events = events or EventBus()
        self.hasher = Hasher(lib)
        self.chain = AuditChain(self.hasher, gateway_id)
        self.checkpoints = checkpoint_writer or CheckpointWriter(None)
        self.checkpoint_every_events = checkpoint_every_events
        self.checkpoint_every_ms = int(checkpoint_every_s * 1000)
        self.tick_interval_s = tick_interval_s
        self.warm_start_windows = warm_start_windows
        self.retention_days = retention_days
        self.archive_dir = Path(archive_dir) if archive_dir is not None else None
        self.clock = clock
        self.engines: dict[str, NodeEngine] = {}
        self.node_source: dict[str, str] = {}
        self.idem = IdempotencyGuard()
        fmask = 0
        for i, name in enumerate(FEATURES):
            if policy.effective.get(f"health.features.{name}", 1):
                fmask |= 1 << i
        self.capture = BaselineCapture(lib, fmask)
        self.min_baseline_samples = int(policy.effective.get("baseline.min_samples", 10))
        self.stats = PipelineStats()
        self._q: queue.Queue = queue.Queue(maxsize=50000)
        self._thread: threading.Thread | None = None
        self._running = False
        self._last_cp_seq = 0
        self._last_cp_ms = 0
        self._retention_next_ms: int | None = None
        self._retention_busy = False        # archive being written or prune batches pending
        self._prune_job: retention.PruneJob | None = None
        self._writer_lock: WriterLock | None = None     # held from open() to close()
        self.started_ms: int | None = None

    # ================================================================ lifecycle
    def open(self) -> None:
        """Take the database's writer lock, load the audit head, warm up engines from
        history (no thread).  WriterLockHeld, before anything is read or written, while
        another pipeline (the gateway, a write command) has the database open."""
        lock = WriterLock(self.db.path)
        lock.acquire()
        try:
            self.chain.load_head(self.db.writer)
            row = self.db.writer.execute("SELECT seq, ts_ms FROM checkpoints ORDER BY seq DESC "
                                         "LIMIT 1").fetchone()
            if row:
                self._last_cp_seq, self._last_cp_ms = int(row["seq"]), int(row["ts_ms"])
            self._warm_start()
            self.idem.warm(self.db.writer)
            self.started_ms = self.clock()
            self._retention_next_ms = self.started_ms + retention.FIRST_RUN_DELAY_MS
            self._audit_simple("gateway.started", None, {
                "engine_version": self.lib.version, "policy_hash": self.policy.hash,
                "policy_name": self.policy.meta.get("name"), "nodes": sorted(self.engines)})
        except BaseException:
            lock.release()
            raise
        self._writer_lock = lock

    def start(self) -> None:
        self.open()
        self.start_worker()

    def start_worker(self) -> None:
        """Start the worker thread on an already opened pipeline."""
        self._running = True
        self._thread = threading.Thread(target=self._run, name="algaops-pipeline", daemon=True)
        self._thread.start()

    def stop(self, timeout: float = 10.0) -> None:
        if self._thread is None:
            self.close()
            return
        self._q.put(_STOP)
        self._thread.join(timeout)
        self._thread = None

    def close(self) -> None:
        """Record the stop and checkpoint, then release the writer lock.  Writes nothing
        unless open() succeeded (a refused start, a second close)."""
        lock, self._writer_lock = self._writer_lock, None
        try:
            if lock is not None:
                if self._prune_job is not None:
                    log.warning("retention: stopped while pruning; the next run archives and "
                                "prunes the remaining rows")
                self._audit_simple("gateway.stopped", None, {"stats": self.stats.snapshot()})
                self._maybe_checkpoint(force=True)
        finally:
            for e in self.engines.values():
                e.close()
            self.engines.clear()
            if lock is not None:
                lock.release()

    def _warm_start(self) -> None:
        conn = self.db.writer
        for node in repo.list_nodes(conn):
            nid = node["node_id"]
            self.node_source[nid] = node["data_source"]
            self._replay_history(self._new_engine(nid), nid)
            log.info("warm start: node %s restored from history", nid)

    def _replay_history(self, eng: NodeEngine, node_id: str) -> None:
        """Rebuild engine state from stored windows, then re-apply what window replay
        cannot reproduce: a stream timeout still in force and the open alert."""
        conn = self.db.writer
        for r in repo.recent_windows(conn, node_id, self.warm_start_windows):
            try:
                nw = NormalizedWindow.from_record(json.loads(r["record_json"]))
                eng.process(nw.to_c())
            except Exception:  # corrupt history must not stop the gateway
                log.exception("warm start: skipping unreadable window %s", r["id"])
        last = repo.last_decision(conn, node_id)
        if last is not None and last["kind"] == "stream_timeout":
            eng.tick(int(last["decided_ms"]))    # already reported: no second timeout
        current = repo.current_open_alert(conn, node_id)
        eng.restore_alert(current["kind"] if current is not None else None)

    def _new_engine(self, node_id: str) -> NodeEngine:
        eng = NodeEngine(self.lib, self.policy, node_id)
        for b in repo.active_baselines(self.db.writer, node_id):
            try:
                eng.set_baseline(stats_to_baseline(json.loads(b["stats_json"]), int(b["id"])))
            except Exception:
                log.exception("active baseline %s rejected by the core", b["id"])
        self.engines[node_id] = eng
        return eng

    def _engine(self, node_id: str) -> NodeEngine:
        return self.engines.get(node_id) or self._new_engine(node_id)

    def _rebuild_engine(self, node_id: str) -> None:
        old = self.engines.pop(node_id, None)
        if old:
            old.close()
        self._replay_history(self._new_engine(node_id), node_id)

    # ================================================================ worker
    def submit_message(self, topic: str, payload: bytes, received_ms: int,
                       ack: Callable[[], None] | None = None) -> bool:
        try:
            self._q.put_nowait(("msg", topic, payload, received_ms, ack))
            return True
        except queue.Full:
            self.stats.queue_full += 1
            log.error("pipeline queue full: message not acknowledged, broker will redeliver")
            return False

    def call(self, fn: Callable[[Pipeline], Any], timeout: float = 60.0) -> Any:
        """Run fn(pipeline) on the worker thread (serialised with ingestion)."""
        if self._thread is None or threading.current_thread() is self._thread:
            return fn(self)
        fut: Future = Future()
        self._q.put(("cmd", fn, fut))
        return fut.result(timeout)

    def submit(self, fn: Callable[[Pipeline], Any]) -> None:
        """Queue fn(pipeline) for the worker without waiting (dropped if it stops first)."""
        self._q.put(("cmd", fn, Future()))

    def _run(self) -> None:
        next_tick = time.monotonic() + self.tick_interval_s
        while True:
            timeout = max(0.0, next_tick - time.monotonic())
            try:
                item = self._q.get(timeout=timeout)
            except queue.Empty:
                item = None
            if item is _STOP:
                break
            if item is not None:
                self._dispatch(item)
            if time.monotonic() >= next_tick:
                try:
                    self.tick(self.clock())
                except Exception:
                    log.exception("tick failed")
                next_tick = time.monotonic() + self.tick_interval_s
        self.close()

    def _dispatch(self, item) -> None:
        kind = item[0]
        if kind == "msg":
            _, topic, payload, received_ms, ack = item
            ok = False
            try:
                ok = self.handle_message(topic, payload, received_ms)
            except Exception:
                log.exception("message handling failed (topic %s)", topic)
            if ok and ack:
                ack()
        elif kind == "cmd":
            _, fn, fut = item
            try:
                fut.set_result(fn(self))
            except Exception as exc:
                fut.set_exception(exc)

    # ================================================================ handlers
    def handle_message(self, topic: str, payload: bytes, received_ms: int) -> bool:
        """Process one MQTT message; returns True when it may be acknowledged."""
        self.stats.received += 1
        parsed = parse_topic(topic, self.topic_prefix)
        raw_digest = self.hasher.hexdigest(payload)
        if parsed is None:
            self._reject(None, topic, payload, raw_digest, "topic outside the AlgaOps contract",
                         received_ms)
            return True
        kind, node_id = parsed
        if kind == "status":
            return self._handle_status(node_id, topic, payload, raw_digest, received_ms)
        try:
            nw = parse_telemetry(payload, received_ms, node_id)
        except PayloadError as exc:
            self._reject(node_id, topic, payload, raw_digest, exc.reason, received_ms)
            return True
        if self.idem.is_duplicate(node_id, nw.boot_id, nw.seq, raw_digest):
            self._duplicate(node_id, topic, raw_digest, received_ms)
            return True
        return self._process_window(nw, raw_digest)

    def _duplicate(self, node_id: str, topic: str, raw_digest: str, received_ms: int) -> None:
        self.stats.duplicate_deliveries += 1
        with self.db.transaction() as tx:
            repo.insert_ingest_log(tx, received_ms, topic, node_id, "duplicate_delivery",
                                   "exact redelivery of an already processed window",
                                   raw_digest, None)
        self.events.publish("packet", {"node_id": node_id, "status": "duplicate_delivery"})

    def _process_window(self, nw: NormalizedWindow, raw_digest: str) -> bool:
        node = nw.node_id
        eng = self._engine(node)
        cw = nw.to_c()
        d = eng.process(cw)
        decided = self.clock()
        record = nw.to_record()
        record_json = canonical_json(record)
        record_digest = self.hasher.hexdigest(record_json.encode("utf-8"))
        rec = DecisionRecord.from_core(self.lib, d, node_id=node, data_source=nw.source,
                                       decided_ms=decided, policy_hash=self.policy.hash,
                                       window_digest=record_digest)
        decision_json = rec.canonical()
        decision_digest = self.hasher.hexdigest(decision_json.encode("utf-8"))
        latency = float(max(0, decided - nw.received_ms))
        alert_info: dict[str, Any] | None = None
        try:
            with self.db.transaction() as tx:
                repo.upsert_node(tx, node, nw.source, nw.received_ms, nw.boot_id, nw.fw)
                window_id = repo.insert_window(
                    tx, node_id=node, boot_id=nw.boot_id, seq=nw.seq, t_end_ms=nw.t_end_ms,
                    received_ms=nw.received_ms, data_source=nw.source, payload_digest=raw_digest,
                    record_json=record_json, record_digest=record_digest, record=record)
                diag_id = repo.insert_diagnostic(tx, window_id=window_id, doc=rec.doc,
                                                 decision_json=decision_json,
                                                 decision_digest=decision_digest,
                                                 latency_ms=latency)
                if d.missing_count:
                    repo.insert_gap(tx, node, nw.boot_id, nw.seq, int(d.missing_count), decided)
                ev = self.chain.append(tx, "window.diagnosed", decided, node, {
                    "window_id": window_id, "window_digest": record_digest,
                    "payload_digest": raw_digest, "decision_id": diag_id,
                    "decision_digest": decision_digest, "boot_id": nw.boot_id,
                    "node_seq": nw.seq, "outcome": rec.outcome, "trust_score": rec.doc["trust"]["score"],
                    "flags": rec.doc["trust"]["flags"], "mode": rec.mode,
                    "data_source": nw.source, "policy_hash": self.policy.hash})
                repo.set_diagnostic_audit_seq(tx, diag_id, ev.seq)
                alert_info = self._alerts(tx, rec, diag_id, decided)
        except repo.DuplicateWindow:
            self.chain.rollback()
            self._rebuild_engine(node)   # the engine saw this window twice
            self.idem.remember(node, nw.boot_id, nw.seq, raw_digest)
            self._duplicate(node, "", raw_digest, nw.received_ms)
            return True
        except Exception:
            self.chain.rollback()
            self.stats.storage_errors += 1
            log.exception("storage failed for %s seq %s; not acknowledged", node, nw.seq)
            self._rebuild_engine(node)
            return False
        self.chain.commit()
        self.idem.remember(node, nw.boot_id, nw.seq, raw_digest)
        self.node_source[node] = nw.source
        self.stats.accepted += 1
        self.stats.note_latency(latency)
        self._after_decision(rec, diag_id, window_id, alert_info)
        done = self.capture.offer(node, cw, d)
        if done is not None:
            self._finish_capture(done)
        elif node in self.capture.sessions:
            self.events.publish("baseline", {"capture": self.capture.sessions[node].status()})
        self._maybe_checkpoint()
        return True

    def _alerts(self, tx, rec: DecisionRecord, diag_id: int, ts: int) -> dict[str, Any] | None:
        node = rec.doc["node_id"]
        event = rec.alert_event
        reason = rec.doc["reason"]
        current = repo.current_open_alert(tx, node)
        if event == "OPENED":
            if current is not None:   # defensive: never two open alerts per node
                repo.close_alert(tx, int(current["id"]), "superseded", ts, diag_id)
            kind = rec.doc["alert"]["kind"]
            aid = repo.open_alert(tx, node, kind, ts, diag_id, reason)
            self.chain.append(tx, "alert.opened", ts, node, {
                "alert_id": aid, "kind": kind, "opened_ms": ts, "decision_id": diag_id,
                "reason": reason})
            return {"event": "opened", "alert_id": aid, "kind": kind}
        if event == "CHANGED":
            prev_id = int(current["id"]) if current is not None else None
            if prev_id is not None:
                repo.close_alert(tx, prev_id, "superseded", ts, diag_id)
            kind = rec.doc["alert"]["kind"]
            aid = repo.open_alert(tx, node, kind, ts, diag_id, reason)
            self.chain.append(tx, "alert.changed", ts, node, {
                "alert_id": aid, "prev_alert_id": prev_id, "kind": kind,
                "prev_kind": rec.doc["alert"]["prev_kind"], "opened_ms": ts,
                "decision_id": diag_id, "reason": reason})
            return {"event": "changed", "alert_id": aid, "kind": kind, "prev_alert_id": prev_id}
        if event == "CLEARED":
            if current is None:
                return None
            repo.close_alert(tx, int(current["id"]), "cleared", ts, diag_id)
            self.chain.append(tx, "alert.cleared", ts, node, {
                "alert_id": int(current["id"]), "kind": current["kind"], "closed_ms": ts,
                "decision_id": diag_id})
            return {"event": "cleared", "alert_id": int(current["id"]), "kind": current["kind"]}
        if current is not None and rec.outcome == current["kind"]:
            repo.touch_alert(tx, int(current["id"]), reason)
        return None

    def _after_decision(self, rec: DecisionRecord, diag_id: int, window_id: int | None,
                        alert_info: dict[str, Any] | None) -> None:
        d = rec.doc
        self.events.publish("decision", {
            "id": diag_id, "window_id": window_id, "node_id": d["node_id"], "kind": d["kind"],
            "outcome": d["outcome"], "trust_score": d["trust"]["score"],
            "flags": d["trust"]["flags"], "mode": d["mode"], "method": d["method"],
            "seq": d["seq"], "timestamp_ms": d["timestamp_ms"], "received_ms": d["received_ms"],
            "data_source": d["data_source"], "features": d["features"],
            "reason": d["reason"][:300], "alert_event": d["alert"]["event"]})
        if alert_info:
            self.events.publish("alert", {"node_id": d["node_id"], **alert_info})

    def _reject(self, node_id: str | None, topic: str, payload: bytes, raw_digest: str,
                reason: str, received_ms: int) -> None:
        self.stats.rejected += 1
        excerpt = payload[:200].decode("utf-8", errors="replace")
        rec = None
        alert_info = None
        diag_id = None
        try:
            with self.db.transaction() as tx:
                repo.insert_ingest_log(tx, received_ms, topic, node_id, "rejected", reason,
                                       raw_digest, excerpt)
                body: dict[str, Any] = {"topic": topic[:200], "reason": reason,
                                        "raw_digest": raw_digest}
                if node_id is not None:
                    eng = self._engine(node_id)
                    d = eng.malformed(received_ms, reason)
                    src = self.node_source.get(node_id, "unknown")
                    rec = DecisionRecord.from_core(self.lib, d, node_id=node_id, data_source=src,
                                                   decided_ms=self.clock(),
                                                   policy_hash=self.policy.hash,
                                                   window_digest=None)
                    dj = rec.canonical()
                    dd = self.hasher.hexdigest(dj.encode("utf-8"))
                    repo.upsert_node(tx, node_id, src, received_ms)
                    diag_id = repo.insert_diagnostic(tx, window_id=None, doc=rec.doc,
                                                     decision_json=dj, decision_digest=dd,
                                                     latency_ms=None)
                    body.update({"decision_id": diag_id, "decision_digest": dd,
                                 "outcome": rec.outcome})
                ev = self.chain.append(tx, "ingest.rejected", received_ms, node_id, body)
                if diag_id is not None and rec is not None:
                    repo.set_diagnostic_audit_seq(tx, diag_id, ev.seq)
                    alert_info = self._alerts(tx, rec, diag_id, received_ms)
        except Exception:
            self.chain.rollback()
            self.stats.storage_errors += 1
            log.exception("could not record rejected message")
            return
        self.chain.commit()
        log.warning("rejected message on %s: %s", topic, reason)
        self.events.publish("packet", {"node_id": node_id, "status": "rejected", "reason": reason})
        if rec is not None and diag_id is not None:
            self._after_decision(rec, diag_id, None, alert_info)

    def _handle_status(self, node_id: str, topic: str, payload: bytes, raw_digest: str,
                       received_ms: int) -> bool:
        try:
            st = parse_status(payload, received_ms, node_id)
        except PayloadError as exc:
            self.stats.rejected += 1
            with self.db.transaction() as tx:
                repo.insert_ingest_log(tx, received_ms, topic, node_id, "rejected", exc.reason,
                                       raw_digest, payload[:200].decode("utf-8", "replace"))
            return True
        src = st.fields.get("source") or self.node_source.get(node_id, "unknown")
        with self.db.transaction() as tx:
            prev = repo.update_node_status(tx, node_id, st.to_record(), received_ms, src)
            if prev != st.state:
                self.chain.append(tx, "node.status", received_ms, node_id, {
                    "state": st.state, "previous": prev, "boot_id": st.fields.get("boot_id"),
                    "raw_digest": raw_digest})
        self.chain.commit()
        self.events.publish("node", {"node_id": node_id, "state": st.state,
                                     "status": st.to_record()})
        return True

    # ================================================================ tick
    def tick(self, now: int) -> int:
        emitted = 0
        for node, eng in list(self.engines.items()):
            d = eng.tick(now)
            if d is None:
                continue
            emitted += 1
            self.stats.stream_timeouts += 1
            src = self.node_source.get(node, "unknown")
            rec = DecisionRecord.from_core(self.lib, d, node_id=node, data_source=src,
                                           decided_ms=now, policy_hash=self.policy.hash,
                                           window_digest=None)
            dj = rec.canonical()
            dd = self.hasher.hexdigest(dj.encode("utf-8"))
            alert_info = None
            try:
                with self.db.transaction() as tx:
                    diag_id = repo.insert_diagnostic(tx, window_id=None, doc=rec.doc,
                                                     decision_json=dj, decision_digest=dd,
                                                     latency_ms=None)
                    ev = self.chain.append(tx, "stream.timeout", now, node, {
                        "decision_id": diag_id, "decision_digest": dd, "outcome": rec.outcome,
                        "explanation": rec.doc["trust"]["explanation"]})
                    repo.set_diagnostic_audit_seq(tx, diag_id, ev.seq)
                    alert_info = self._alerts(tx, rec, diag_id, now)
            except Exception:
                self.chain.rollback()
                self.stats.storage_errors += 1
                log.exception("could not record stream timeout for %s", node)
                continue
            self.chain.commit()
            self._after_decision(rec, diag_id, None, alert_info)
        self._maybe_checkpoint(now=now)
        self._retention_tick(now)
        return emitted

    # ================================================================ retention
    def _retention_tick(self, now: int) -> None:
        """One prune batch per tick while a run is pruning; start the daily run when due."""
        if self._prune_job is not None:
            job = self._prune_job
            try:
                done = job.step(self)
                if done:
                    job.finish(self)
            except Exception:
                log.exception("retention: prune batch failed; the next run archives the rest "
                              "again")
                done = True
            if done:
                self._prune_job, self._retention_busy = None, False
            return
        if (self.retention_days <= 0 or self._retention_busy or self._retention_next_ms is None
                or now < self._retention_next_ms):
            return
        self._retention_next_ms = now + retention.DAY_MS
        if self._thread is not None and threading.current_thread() is self._thread:
            self._retention_busy = True
            self._maybe_checkpoint(force=True, now=now)    # the archive holds its head
            threading.Thread(target=retention.run_in_background,
                             args=(self, self.retention_days, now), name="algaops-retention",
                             daemon=True).start()
            return
        try:                                               # no worker thread (offline): inline
            retention.run(self, self.retention_days, now)
        except Exception:
            log.exception("retention run failed")

    def start_prune(self, job: retention.PruneJob | None) -> None:
        """Worker side of the daily run: its archive is written (or the run found nothing)."""
        self._prune_job = job
        self._retention_busy = job is not None

    def prune_now(self, retention_days: int | None = None) -> dict[str, Any]:
        """Archive, then prune rows older than retention_days and wait for it (CLI `prune`,
        gateway stopped).  Not from the worker thread."""
        days = self.retention_days if retention_days is None else retention_days
        if days < 1:
            raise ValueError("retention_days must be >= 1 to prune")
        if self._retention_busy:
            raise RuntimeError("a retention run is already in progress")
        return retention.run(self, days)

    def _maybe_checkpoint(self, force: bool = False, now: int | None = None) -> None:
        seq, h = self.chain.head
        if seq == 0 or seq == self._last_cp_seq:
            return
        now = self.clock() if now is None else now
        due = (force or seq - self._last_cp_seq >= self.checkpoint_every_events or
               now - self._last_cp_ms >= self.checkpoint_every_ms)
        if not due:
            return
        cp = Checkpoint(seq, h, now)
        try:
            with self.db.transaction() as tx:
                self.checkpoints.write(tx, cp)
        except Exception:
            log.exception("checkpoint write failed")
            return
        self._last_cp_seq, self._last_cp_ms = seq, now
        self.events.publish("audit", {"checkpoint_seq": seq, "head_hash": h})

    # ================================================================ commands
    def _audit_simple(self, etype: str, node: str | None, body: dict[str, Any],
                      ts: int | None = None) -> int:
        ts = self.clock() if ts is None else ts
        with self.db.transaction() as tx:
            ev = self.chain.append(tx, etype, ts, node, body)
        self.chain.commit()
        return ev.seq

    def record_audit(self, etype: str, node: str | None, body: dict[str, Any]) -> int:
        return self.call(lambda p: p._audit_simple(etype, node, body))

    def checkpoint_now(self) -> None:
        """Checkpoint the current head (database + external file), e.g. before an export."""
        self.call(lambda p: p._maybe_checkpoint(force=True))

    def start_capture(self, node_id: str, mode: str, windows: int,
                      activate: bool = False) -> dict[str, Any]:
        def fn(p: Pipeline):
            s = p.capture.start(node_id, mode, windows, p.clock())
            s.auto_activate = activate  # type: ignore[attr-defined]
            return s.status()
        return self.call(fn)

    def cancel_capture(self, node_id: str) -> bool:
        return self.call(lambda p: p.capture.cancel(node_id) is not None)

    def capture_status(self) -> list[dict[str, Any]]:
        return self.call(lambda p: [s.status() for s in p.capture.sessions.values()])

    def _finish_capture(self, s) -> None:
        try:
            bl = self.capture.finish(s, self.min_baseline_samples)
        except Exception as exc:
            log.warning("baseline capture for %s failed: %s", s.node_id, exc)
            self.events.publish("baseline", {"node_id": s.node_id, "error": str(exc)})
            return
        stats = baseline_to_stats(bl)
        source = {"kind": "capture", "first_seq": s.first_seq, "last_seq": s.last_seq,
                  "accepted": s.accepted, "skipped_untrusted": s.skipped_untrusted,
                  "skipped_other_mode": s.skipped_mode, "started_ms": s.started_ms}
        bid = self._store_baseline(s.node_id, stats, source, "baseline.captured")
        if getattr(s, "auto_activate", False):
            self._activate(bid)

    def _store_baseline(self, node_id: str, stats: dict[str, Any], source: dict[str, Any],
                        etype: str) -> int:
        sj = stats_json(stats)
        sd = self.hasher.hexdigest(sj.encode("utf-8"))
        ts = self.clock()
        with self.db.transaction() as tx:
            bid = repo.insert_baseline(tx, node_id, stats["mode"], ts, stats["n"], sj, sd, source)
            self.chain.append(tx, etype, ts, node_id, {
                "baseline_id": bid, "stats_digest": sd, "mode": stats["mode"], "n": stats["n"],
                "features": stats["features"], "source": source})
        self.chain.commit()
        self.events.publish("baseline", {"node_id": node_id, "baseline_id": bid,
                                         "status": "candidate", "mode": stats["mode"]})
        return bid

    def _activate(self, baseline_id: int) -> dict[str, Any]:
        b = repo.get_baseline(self.db.writer, baseline_id)
        if b is None:
            raise KeyError(f"baseline {baseline_id} not found")
        stats = json.loads(b["stats_json"])
        cbl = stats_to_baseline(stats, baseline_id)
        ts = self.clock()
        with self.db.transaction() as tx:
            retired = repo.activate_baseline(tx, baseline_id, ts)
            self.chain.append(tx, "baseline.activated", ts, b["node_id"], {
                "baseline_id": baseline_id, "stats_digest": b["stats_digest"],
                "mode": b["mode"], "retired": retired})
        self.chain.commit()
        self._engine(b["node_id"]).set_baseline(cbl)
        self.events.publish("baseline", {"node_id": b["node_id"], "baseline_id": baseline_id,
                                         "status": "active", "mode": b["mode"]})
        return {"baseline_id": baseline_id, "retired": retired}

    def activate_baseline(self, baseline_id: int) -> dict[str, Any]:
        return self.call(lambda p: p._activate(baseline_id))

    def retire_baseline(self, baseline_id: int) -> dict[str, Any]:
        def fn(p: Pipeline):
            b = repo.get_baseline(p.db.writer, baseline_id)
            if b is None:
                raise KeyError(f"baseline {baseline_id} not found")
            ts = p.clock()
            with p.db.transaction() as tx:
                repo.retire_baseline(tx, baseline_id)
                p.chain.append(tx, "baseline.retired", ts, b["node_id"], {
                    "baseline_id": baseline_id, "stats_digest": b["stats_digest"],
                    "mode": b["mode"]})
            p.chain.commit()
            if b["status"] == "active":
                p._engine(b["node_id"]).clear_baseline(mode_index(b["mode"]))
            return {"baseline_id": baseline_id, "status": "retired"}
        return self.call(fn)

    def import_baseline_csv(self, node_id: str, csv_path: str, activate: bool = False
                            ) -> list[int]:
        def fn(p: Pipeline):
            digest = p.hasher.hexdigest(open(csv_path, "rb").read())
            ids = []
            for bl in baselines_from_csv(p.lib, p.policy, csv_path):
                stats = baseline_to_stats(bl)
                source = {"kind": "csv", "file": str(csv_path).replace("\\", "/").split("/")[-1],
                          "file_digest": digest}
                bid = p._store_baseline(node_id, stats, source, "baseline.imported")
                if activate:
                    p._activate(bid)
                ids.append(bid)
            return ids
        return self.call(fn)

    def record_feedback(self, alert_id: int, verdict: str, note: str | None,
                        technician: str | None) -> dict[str, Any]:
        fb = validate_feedback(alert_id, verdict, note, technician)

        def fn(p: Pipeline):
            alert = p.db.writer.execute("SELECT node_id FROM alerts WHERE id = ?",
                                        (fb.alert_id,)).fetchone()
            if alert is None:
                raise KeyError(f"alert {fb.alert_id} not found")
            ts = p.clock()
            with p.db.transaction() as tx:
                fid = repo.insert_feedback(tx, fb.alert_id, fb.verdict, fb.note, fb.technician, ts)
                p.chain.append(tx, "feedback.recorded", ts, alert["node_id"], {
                    "feedback_id": fid, "alert_id": fb.alert_id, "verdict": fb.verdict,
                    "note": fb.note, "technician": fb.technician})
            p.chain.commit()
            p.events.publish("alert", {"node_id": alert["node_id"], "event": "feedback",
                                       "alert_id": fb.alert_id, "verdict": fb.verdict})
            return {"feedback_id": fid, "alert_id": fb.alert_id, "verdict": fb.verdict}
        return self.call(fn)
