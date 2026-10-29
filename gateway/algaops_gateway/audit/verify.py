"""Verification of the audit chain and of every row it references.

Detects: modified events (HASH_MISMATCH), deleted or inserted events
(SEQUENCE_GAP / BROKEN_LINK), reordered events (BROKEN_LINK), edited or
deleted windows / decisions / baselines / feedback (ROW_MODIFIED /
MISSING_ROW), rows added without an audit event (UNREFERENCED_ROW) and
truncation of the chain below a retained checkpoint (TRUNCATED /
CHECKPOINT_MISMATCH).

Retention (retention.py): a window or decision referenced by an event with
seq <= archived_through_seq and ts_ms < cutoff_ms of an `archive.created`
event may be absent; it is counted as archived, not MISSING_ROW, unless an
alert event names that decision (those are never pruned).  Rows that remain
are checked as before, and so is the whole chain.

Memory does not grow with the chain or with the windows and decisions.  The
gateway writes a window or decision in the transaction of the event that
references it, so referenced ids grow with seq: one pass over the chain meets
the rows of both tables in id order (a merge), and nothing is kept per event
or per row.  A reference that breaks that order is looked up by id and checked
just as strictly.  Kept in memory is only what grows with tables that are
never pruned: the archive.created events and the decision ids that alert
events name (read before the pass, because they decide whether a missing row
was archived), and the alert, feedback and baseline references.  Checkpoints
are compared one at a time.  The work is one snapshot: in the caller's
transaction, or in one of its own.

Limits (reported in every result): an attacker able to rewrite the complete
database AND every retained checkpoint can produce a consistent forgery.
Checkpoints kept off-device (evidence bundles, printed hashes) bound that.
"""

from __future__ import annotations

import bisect
import itertools
import json
import sqlite3
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Callable

from .ascon import ZERO_HASH, Hasher
from .checkpoint import checkpoint_file_size, iter_checkpoint_file

FAILURE_KINDS = {
    "SEQUENCE_GAP", "BROKEN_LINK", "HASH_MISMATCH", "METADATA_MISMATCH", "BAD_PAYLOAD",
    "MISSING_ROW", "ROW_MODIFIED", "UNREFERENCED_ROW", "TRUNCATED", "CHECKPOINT_MISMATCH",
}

LIMITATION = ("Tamper-evident, not tamper-proof: a party able to rewrite the whole database "
              "and every retained checkpoint can forge a consistent chain, and one able to "
              "append to it can declare rows archived (archive.created) and delete them. Keep "
              "checkpoints, evidence bundles and archive bundles off-device.")

ALERT_EVENTS = ("alert.opened", "alert.changed", "alert.cleared")

# Issues are reported group by group in this order (the chain first, then table by
# table), not in the order the single pass meets them; each group keeps at most
# max_issues, and so does the report.
_GROUPS = ("chain", "windows", "windows_missing", "decisions", "decisions_missing", "baselines",
          "window_columns", "feedback", "alerts", "checkpoints")

_INT64 = range(-(1 << 63), 1 << 63)          # what SQLite can hold (and be asked for)


@dataclass
class Issue:
    kind: str
    seq: int | None
    detail: str


@dataclass
class VerificationReport:
    ok: bool = True
    events: int = 0
    head_seq: int = 0
    head_hash: str = ZERO_HASH.hex()
    windows_checked: int = 0
    decisions_checked: int = 0
    other_rows_checked: int = 0
    rows_archived: int = 0
    checkpoints_checked: int = 0
    checkpoint_file: str | None = None
    archives: list[dict[str, Any]] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    issues: list[Issue] = field(default_factory=list)
    limitation: str = LIMITATION

    def to_dict(self) -> dict[str, Any]:
        d = asdict(self)
        d["issues"] = [asdict(i) for i in self.issues]
        return d


class _Issues:
    """Issues by group; close() lists them in _GROUPS order, at most max_issues."""

    def __init__(self, rep: VerificationReport, max_issues: int) -> None:
        self._rep = rep
        self._max = max(max_issues, 0)
        self._groups: dict[str, list[Issue]] = {g: [] for g in _GROUPS}

    def add(self, group: str, kind: str, seq: int | None, detail: str) -> None:
        if kind in FAILURE_KINDS:
            self._rep.ok = False
        issues = self._groups[group]
        if len(issues) < self._max:
            issues.append(Issue(kind, seq, detail))

    def close(self) -> None:
        self._rep.issues = list(itertools.islice(
            itertools.chain.from_iterable(self._groups[g] for g in _GROUPS), self._max))


Ref = tuple[int, str, int, Any]    # (event seq, row digest, event ts_ms, decision id)


def _ref(refs: dict[int, Ref], key: Any, digest: Any, seq: int, ts: int,
         decision_id: Any = None) -> None:
    if isinstance(key, int) and isinstance(digest, str):
        refs[key] = (seq, digest, ts, decision_id)


def _is_int(x: Any) -> bool:
    return isinstance(x, int) and not isinstance(x, bool)


def _object(payload: str) -> dict:
    """The payload as a JSON object; {} if it is not one (the pass reports BAD_PAYLOAD)."""
    try:
        body = json.loads(payload)
    except (ValueError, RecursionError):
        return {}
    return body if isinstance(body, dict) else {}


class _Archives:
    """(archived_through_seq, cutoff_ms) of every archive.created event."""

    def __init__(self) -> None:
        self.events: list[tuple[int, int]] = []
        self._through: list[int] = []
        self._cutoff: list[int] = []       # max cutoff over the events from index i on

    def freeze(self) -> None:
        ev = sorted(self.events)
        self._through = [t for t, _ in ev]
        self._cutoff = list(itertools.accumulate(reversed([c for _, c in ev]), max))[::-1]

    def covers(self, seq: int, ts: int) -> bool:
        i = bisect.bisect_left(self._through, seq)
        return i < len(self._through) and ts < self._cutoff[i]


def _archive_event(rep: VerificationReport, archives: _Archives, seq: int, body: dict) -> None:
    through, cutoff = body.get("archived_through_seq"), body.get("cutoff_ms")
    if not (_is_int(through) and _is_int(cutoff) and 0 <= through < seq):
        rep.notes.append(f"archive.created at seq {seq} is malformed and was ignored")
        return
    archives.events.append((through, cutoff))
    rep.archives.append({"seq": seq, "cutoff_ms": cutoff, "archived_through_seq": through,
                         "file": body.get("file"), "bundle_digest": body.get("bundle_digest"),
                         "bundle_head_seq": body.get("bundle_head_seq"),
                         "bundle_head_hash": body.get("bundle_head_hash")})


def _read_archives(conn: sqlite3.Connection, rep: VerificationReport
                   ) -> tuple[_Archives, set[Any]]:
    """archive.created events and the decision ids alert events name.  Read before the
    pass: an archive event comes after the events it covers, and a missing row is judged
    when its event is met.  Selected by the type column, like the pass, which reports a
    type that differs from the hashed payload (METADATA_MISMATCH)."""
    archives, named = _Archives(), set()
    types = ("archive.created", *ALERT_EVENTS)
    for r in conn.execute("SELECT seq, type, payload FROM audit_events WHERE type IN "
                          f"({', '.join('?' * len(types))}) ORDER BY seq", types):
        body = _object(r["payload"])
        if r["type"] == "archive.created":
            _archive_event(rep, archives, int(r["seq"]), body)
        elif isinstance(body.get("decision_id"), int):
            named.add(body["decision_id"])
    archives.freeze()
    return archives, named


class _Rows:
    """The rows of one table that events reference by id and digest (windows, decisions),
    read in id order alongside the chain: a reference above the previous one advances the
    table (rows passed over have no event), any other is looked up by id."""

    def __init__(self, conn: sqlite3.Connection, hasher: Hasher, issues: _Issues,
                 archives: _Archives, alert_decisions: set[Any], label: str, group: str,
                 select: str, on_row: Callable[[sqlite3.Row], None] | None = None) -> None:
        self._conn, self._hasher, self._issues = conn, hasher, issues
        self._archives, self._alert_decisions = archives, alert_decisions
        self._label, self._group, self._on_row = label, group, on_row
        self._by_id = select + " WHERE id = ?"
        self._rows = conn.execute(select + " ORDER BY id")
        self._row = self._rows.fetchone()
        self._last: int | None = None
        self.checked = 0
        self.archived = 0

    def _next(self) -> None:
        self.checked += 1
        if self._on_row is not None:
            self._on_row(self._row)
        self._row = self._rows.fetchone()

    def _unreferenced(self) -> None:
        self._issues.add(self._group, "UNREFERENCED_ROW", None,
                         f"{self._label} {self._row['id']} has no audit event")
        self._next()

    def ref(self, rid: int, seq: int, digest: str, ts: Any, decision_id: Any) -> None:
        if self._last is None or rid > self._last:
            self._last = rid
            while self._row is not None and self._row["id"] < rid:
                self._unreferenced()
            row = self._row if self._row is not None and self._row["id"] == rid else None
            if row is not None:
                self._next()
        else:
            row = self._conn.execute(self._by_id, (rid,)).fetchone() if rid in _INT64 else None
        if row is None:
            try:
                named = decision_id in self._alert_decisions
            except TypeError:                # unhashable, so no alert event names it
                named = False
            if _is_int(ts) and self._archives.covers(seq, ts) and not named:
                self.archived += 1
            else:
                self._issues.add(self._group + "_missing", "MISSING_ROW", seq,
                                 f"{self._label} {rid} referenced by the chain is missing")
            return
        actual = self._hasher.hexdigest(row["t"].encode("utf-8"))
        if actual != digest or row["d"] != digest:
            self._issues.add(self._group, "ROW_MODIFIED", seq,
                             f"{self._label} {rid} differs from its audited digest")

    def finish(self) -> int:
        while self._row is not None:
            self._unreferenced()
        return self.checked


def verify_database(conn: sqlite3.Connection, hasher: Hasher,
                    checkpoint_file: str | Path | None = None, max_issues: int = 100, *,
                    checkpoint_size: int | None = None) -> VerificationReport:
    """checkpoint_size: compare the external checkpoint file up to this many bytes, its
    size when the caller's read snapshot began (see checkpoint.py); by default its size
    before this function's first query."""
    rep = VerificationReport()
    issues = _Issues(rep, max_issues)
    first_notes: list[str] = []
    if checkpoint_file:
        rep.checkpoint_file = str(checkpoint_file)
        if checkpoint_size is None:
            checkpoint_size = checkpoint_file_size(checkpoint_file)
        if not Path(checkpoint_file).exists():
            first_notes.append("external checkpoint file not found")
    own_snapshot = not conn.in_transaction
    if own_snapshot:
        conn.execute("BEGIN")
    try:
        _verify(conn, hasher, rep, issues, checkpoint_file, checkpoint_size, first_notes)
    finally:
        if own_snapshot:
            conn.execute("COMMIT")
    issues.close()
    return rep


def _verify(conn: sqlite3.Connection, hasher: Hasher, rep: VerificationReport, issues: _Issues,
            checkpoint_file: str | Path | None, checkpoint_size: int | None,
            first_notes: list[str]) -> None:
    archives, alert_decisions = _read_archives(conn, rep)

    def window_columns(row: sqlite3.Row) -> None:
        """Denormalised window columns must match the hashed record."""
        try:
            rec = json.loads(row["t"])
        except (ValueError, RecursionError):
            return
        if not isinstance(rec, dict):
            return
        cols = (rec.get("node_id"), rec.get("boot_id"), rec.get("seq"), rec.get("vib_rms_g"),
                rec.get("temp_c"), rec.get("current_a"))
        if cols != (row["node_id"], row["boot_id"], row["seq"], row["vib_rms_g"], row["temp_c"],
                    row["current_a"]):
            issues.add("window_columns", "ROW_MODIFIED", None,
                       f"window {row['id']} indexed columns differ from record")

    windows = _Rows(conn, hasher, issues, archives, alert_decisions, "window", "windows",
                    "SELECT id, record_json AS t, record_digest AS d, vib_rms_g, temp_c, "
                    "current_a, node_id, boot_id, seq FROM windows", window_columns)
    decisions = _Rows(conn, hasher, issues, archives, alert_decisions, "decision", "decisions",
                      "SELECT id, decision_json AS t, decision_digest AS d FROM diagnostics")
    baselines: dict[int, Ref] = {}
    feedback: dict[int, tuple[int, tuple]] = {}     # id -> (seq, audited column values)
    alerts: dict[int, tuple[int, tuple]] = {}

    expected = 1
    prev = ZERO_HASH.hex()
    for r in conn.execute("SELECT seq, ts_ms, type, node_id, payload, prev_hash, hash "
                          "FROM audit_events ORDER BY seq"):
        seq = int(r["seq"])
        rep.events += 1
        if seq != expected:
            issues.add("chain", "SEQUENCE_GAP", seq, f"expected seq {expected}, found {seq}")
        if r["prev_hash"] != prev:
            issues.add("chain", "BROKEN_LINK", seq, "prev_hash does not match the previous event")
        payload = r["payload"].encode("utf-8")
        try:
            recomputed = hasher.link(bytes.fromhex(r["prev_hash"]), seq, payload).hex()
        except ValueError:
            recomputed = ""
        if recomputed != r["hash"]:
            issues.add("chain", "HASH_MISMATCH", seq, "event content or hash was modified")
        try:
            body = json.loads(r["payload"])
        except (ValueError, RecursionError):
            issues.add("chain", "BAD_PAYLOAD", seq, "payload is not valid JSON")
            body = {}
        if not isinstance(body, dict):
            issues.add("chain", "BAD_PAYLOAD", seq, "payload is not a JSON object")
            body = {}
        if body and (body.get("type") != r["type"] or body.get("seq") != seq or
                     body.get("node_id") != r["node_id"] or body.get("ts_ms") != r["ts_ms"]):
            issues.add("chain", "METADATA_MISMATCH", seq,
                       "indexed columns differ from the hashed payload")
        t, ts = r["type"], r["ts_ms"]
        wid, wdigest = body.get("window_id"), body.get("window_digest")
        if isinstance(wid, int) and isinstance(wdigest, str):
            windows.ref(wid, seq, wdigest, ts, body.get("decision_id"))
        did, ddigest = body.get("decision_id"), body.get("decision_digest")
        if isinstance(did, int) and isinstance(ddigest, str):
            decisions.ref(did, seq, ddigest, ts, did)
        if t.startswith("baseline."):
            _ref(baselines, body.get("baseline_id"), body.get("stats_digest"), seq, ts)
        if t == "feedback.recorded" and isinstance(body.get("feedback_id"), int):
            feedback[body["feedback_id"]] = (seq, (body.get("alert_id"), body.get("verdict"),
                                                   body.get("note"), body.get("technician")))
        if t in ("alert.opened", "alert.changed") and isinstance(body.get("alert_id"), int):
            alerts[body["alert_id"]] = (seq, (body.get("node_id"), body.get("kind"),
                                              body.get("opened_ms")))
        prev = r["hash"]
        expected = seq + 1
        rep.head_seq, rep.head_hash = seq, r["hash"]

    rep.windows_checked = windows.finish()
    rep.decisions_checked = decisions.finish()
    rep.rows_archived = windows.archived + decisions.archived

    seen = set()
    for row in conn.execute("SELECT id, stats_json AS t, stats_digest AS d FROM baselines"):
        rid = int(row["id"])
        seen.add(rid)
        rep.other_rows_checked += 1
        if rid not in baselines:
            issues.add("baselines", "UNREFERENCED_ROW", None, f"baseline {rid} has no audit event")
            continue
        seq, digest = baselines[rid][:2]
        actual = hasher.hexdigest(row["t"].encode("utf-8"))
        if actual != digest or row["d"] != digest:
            issues.add("baselines", "ROW_MODIFIED", seq,
                       f"baseline {rid} differs from its audited digest")
    for rid, (seq, *_) in baselines.items():
        if rid not in seen:
            issues.add("baselines", "MISSING_ROW", seq,
                       f"baseline {rid} referenced by the chain is missing")

    for row in conn.execute("SELECT id, alert_id, verdict, note, technician FROM feedback"):
        fid = int(row["id"])
        rep.other_rows_checked += 1
        ref = feedback.get(fid)
        if ref is None:
            issues.add("feedback", "UNREFERENCED_ROW", None, f"feedback {fid} has no audit event")
            continue
        seq, audited = ref
        if audited != (row["alert_id"], row["verdict"], row["note"], row["technician"]):
            issues.add("feedback", "ROW_MODIFIED", seq,
                       f"feedback {fid} differs from its audit event")
    for row in conn.execute("SELECT id, node_id, kind, opened_ms FROM alerts"):
        aid = int(row["id"])
        rep.other_rows_checked += 1
        ref = alerts.get(aid)
        if ref is None:
            issues.add("alerts", "UNREFERENCED_ROW", None, f"alert {aid} has no audit event")
            continue
        seq, audited = ref
        if audited != (row["node_id"], row["kind"], row["opened_ms"]):
            issues.add("alerts", "ROW_MODIFIED", seq, f"alert {aid} differs from its audit event")

    def checkpoint(source: str, seq: int, cp_hash: str, chain_hash: str | None) -> None:
        rep.checkpoints_checked += 1
        if seq > rep.head_seq:
            issues.add("checkpoints", "TRUNCATED", seq, f"{source} checkpoint at seq {seq} is "
                       f"beyond the chain head {rep.head_seq}: events were removed")
        elif chain_hash != cp_hash:
            issues.add("checkpoints", "CHECKPOINT_MISMATCH", seq,
                       f"{source} checkpoint hash differs from the chain")

    for r in conn.execute("SELECT c.seq, c.hash, e.hash AS chain_hash FROM checkpoints c "
                          "LEFT JOIN audit_events e ON e.seq = c.seq ORDER BY c.seq"):
        checkpoint("database", int(r["seq"]), r["hash"], r["chain_hash"])
    if checkpoint_file:
        skipped = 0
        for cp in iter_checkpoint_file(checkpoint_file, checkpoint_size):
            if cp is None:
                skipped += 1
                continue
            row = (conn.execute("SELECT hash FROM audit_events WHERE seq = ?", (cp.seq,))
                   .fetchone() if cp.seq in _INT64 else None)
            checkpoint("file", cp.seq, cp.hash, row["hash"] if row is not None else None)
        if skipped:
            first_notes.insert(0, f"{skipped} unreadable checkpoint line(s) skipped "
                               "(e.g. partial write after a crash)")
    rep.notes[:0] = first_notes
    # archive.created records the chain head of its bundle: it must match like a checkpoint.
    for a in rep.archives:
        hs, hh = a["bundle_head_seq"], a["bundle_head_hash"]
        if not (_is_int(hs) and isinstance(hh, str)):
            continue
        row = conn.execute("SELECT hash FROM audit_events WHERE seq = ?", (hs,)).fetchone() \
            if hs in _INT64 else None
        if hs >= a["seq"] or row is None or row["hash"] != hh:
            issues.add("checkpoints", "CHECKPOINT_MISMATCH", a["seq"], "archive.created records "
                       f"bundle head seq {hs} with a hash that differs from the chain")
    if not rep.checkpoints_checked:
        rep.notes.append("no checkpoints yet: truncation of the newest events is not detectable")
    if rep.rows_archived:
        last = rep.archives[-1]
        rep.notes.append(f"{rep.rows_archived} window/decision row(s) were archived and pruned "
                         f"({len(rep.archives)} archive.created event(s), latest at seq "
                         f"{last['seq']}: {last['file']}); they are checked with those bundles")
