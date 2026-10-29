"""Evidence bundle export (zip) — verifiable offline with tools/verify_evidence.py.

Contents:
  manifest.json            evidence_manifest.v1: scope, chain head, file digests,
                           data-source labels, verification result, limitations
  SHA-ASCON256SUMS         "<digest>  <file>" for every other file
  audit_events.jsonl       the complete audit chain (genesis .. head)
  windows.jsonl            {"id", "node_id", "record", "digest"}; record is the
                           exact hashed text
  diagnostics.jsonl        {"id", "window_id", "decision", "digest"}
  alerts.csv, feedback.csv, baselines.jsonl, nodes.json
  gaps.csv, ingest_log.csv detected sequence gaps; rejected messages and exact
                           redeliveries (not chained, covered by SHA-ASCON256SUMS)
  checkpoints.jsonl        DB checkpoints + the external checkpoint file
  verification_report.json gateway-side verification at export time
  README.txt

The whole chain is always included so that every exported row can be checked
against the event that references it; data rows can be scoped by node/time.
A retention archive (retention.py) is such a bundle scoped to everything
before its cutoff.  Secrets (MQTT password, API token) are never part of a bundle.

Each file is streamed from the database into the zip and hashed while it is
written (Ascon-Hash256 init/update/final of the C core), so memory does not grow
with the chain or the rows; neither does verify_database's.  README.txt comes
first, manifest.json and SHA-ASCON256SUMS last, because they list the digests.
"""

from __future__ import annotations

import contextlib
import csv
import json
import sqlite3
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator

from ..audit.ascon import Hasher
from ..audit.checkpoint import checkpoint_file_size, iter_checkpoint_file, iter_db_checkpoints
from ..audit.verify import LIMITATION, verify_database

MANIFEST_SCHEMA = "algaops.evidence_manifest.v1"
CHUNK = 1 << 20     # characters buffered per member before they are hashed and compressed

README = """AlgaOps Assurance evidence bundle
=================================

Verify offline (Python 3.11+, no third-party packages):

    python tools/verify_evidence.py <this-file.zip>

The verifier recomputes every Ascon-Hash256 (NIST SP 800-232) digest with an
independent reference implementation, walks the audit hash chain, and checks
each exported window/decision row against the audit event that references it.

Data labelled "simulator" or "replay" is SYNTHETIC/DEMO data.  Decisions are
inspection warnings for technicians, not validated industrial diagnoses.
""" + "\n" + LIMITATION + "\n"

ALERT_COLS = ["id", "node_id", "kind", "status", "opened_ms", "closed_ms", "open_diag_id",
              "close_diag_id", "summary"]
FEEDBACK_COLS = ["id", "alert_id", "verdict", "note", "technician", "created_ms"]
GAP_COLS = ["id", "node_id", "boot_id", "from_seq", "to_seq", "missing", "detected_ms"]
INGEST_COLS = ["id", "received_ms", "topic", "node_id", "status", "reason", "raw_digest",
               "raw_excerpt"]


@dataclass
class ExportScope:
    node_id: str | None = None
    since_ms: int | None = None
    until_ms: int | None = None

    def where(self, time_col: str) -> tuple[str, list[Any]]:
        clauses, args = [], []
        if self.node_id:
            clauses.append("node_id = ?")
            args.append(self.node_id)
        if self.since_ms is not None:
            clauses.append(f"{time_col} >= ?")
            args.append(self.since_ms)
        if self.until_ms is not None:
            clauses.append(f"{time_col} <= ?")
            args.append(self.until_ms)
        return (" WHERE " + " AND ".join(clauses)) if clauses else "", args


class _Member:
    """Text written into one zip member in pieces; hashed as it goes into the zip."""

    def __init__(self, out, hasher: Hasher) -> None:
        self._out = out
        self._hash = hasher.new()
        self._parts: list[str] = []
        self._size = 0
        self.digest = ""

    def write(self, text: str) -> None:
        self._parts.append(text)
        self._size += len(text)
        if self._size >= CHUNK:
            self._flush()

    def _flush(self) -> None:
        data = "".join(self._parts).encode("utf-8")
        self._parts.clear()
        self._size = 0
        self._hash.update(data)
        self._out.write(data)

    def finish(self) -> None:
        self._flush()
        self.digest = self._hash.hexdigest()


class _BundleWriter:
    """The zip, written member after member; digests of the listed members by name."""

    def __init__(self, zf: zipfile.ZipFile, hasher: Hasher) -> None:
        self._zf = zf
        self._hasher = hasher
        self.digests: dict[str, str] = {}

    @contextlib.contextmanager
    def stream(self, name: str) -> Iterator[_Member]:
        # Member metadata as ZipFile.writestr() sets it.  ZIP64 headers, because the size
        # is not known in advance: audit_events.jsonl grows by ~62 MB per node-day and
        # passes zipfile's 2 GiB limit for plain headers after about 35 node-days.
        info = zipfile.ZipInfo(name, date_time=time.localtime(time.time())[:6])
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o600 << 16
        with self._zf.open(info, "w", force_zip64=True) as out:
            m = _Member(out, self._hasher)
            yield m
            m.finish()
        self.digests[name] = m.digest

    def put(self, name: str, data: bytes, listed: bool = True) -> None:
        self._zf.writestr(name, data)
        if listed:
            self.digests[name] = self._hasher.hexdigest(data)


def _jsonl(m: _Member, rows: Iterable[dict[str, Any]]) -> int:
    n = 0
    for r in rows:
        m.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
        n += 1
    return n


def _csv(m: _Member, header: list[str], rows: Iterable[sqlite3.Row]) -> int:
    w = csv.writer(m, lineterminator="\n")
    w.writerow(header)
    n = 0
    for r in rows:
        w.writerow([r[h] for h in header])
        n += 1
    return n


def build_bundle(conn: sqlite3.Connection, hasher: Hasher, out_path: str | Path, *,
                 gateway_id: str, engine_version: str, policy_hash: str, created_ms: int,
                 scope: ExportScope | None = None, checkpoint_file: str | Path | None = None
                 ) -> dict[str, Any]:
    """Write the bundle from one consistent read snapshot; returns the manifest."""
    scope = scope or ExportScope()
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(out_path.suffix + ".part")
    # Before the snapshot begins: lines appended to the external file later may name
    # events the snapshot does not hold (checkpoint.py).
    cp_size = checkpoint_file_size(checkpoint_file) if checkpoint_file else None
    counts: dict[str, int] = {}
    try:
        with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            bw = _BundleWriter(zf, hasher)
            bw.put("README.txt", README.encode("utf-8"))
            conn.execute("BEGIN")   # snapshot isolation for a read connection in WAL mode
            try:
                a_where, a_args = scope.where("opened_ms")
                with bw.stream("alerts.csv") as m:
                    counts["alerts"] = _csv(m, ALERT_COLS, conn.execute(
                        f"SELECT {', '.join(ALERT_COLS)} FROM alerts{a_where} ORDER BY id",
                        a_args))
                events, head_seq, head_hash = 0, 0, "0" * 64
                with bw.stream("audit_events.jsonl") as m:
                    for r in conn.execute("SELECT seq, ts_ms, type, node_id, payload, prev_hash, "
                                          "hash FROM audit_events ORDER BY seq"):
                        m.write(json.dumps(dict(r), ensure_ascii=False, separators=(",", ":"))
                                + "\n")
                        events += 1
                        head_seq, head_hash = r["seq"], r["hash"]
                with bw.stream("baselines.jsonl") as m:
                    _jsonl(m, ({"id": r["id"], "node_id": r["node_id"], "mode": r["mode"],
                                "status": r["status"], "n": r["n"], "stats": r["stats_json"],
                                "digest": r["stats_digest"]}
                               for r in conn.execute("SELECT * FROM baselines ORDER BY id")))
                with bw.stream("checkpoints.jsonl") as m:
                    counts["checkpoints"] = _jsonl(m, (
                        {"seq": c.seq, "hash": c.hash, "ts_ms": c.ts_ms, "source": "database"}
                        for c in iter_db_checkpoints(conn)))
                    if checkpoint_file:
                        counts["checkpoints"] += _jsonl(m, (
                            {"seq": c.seq, "hash": c.hash, "ts_ms": c.ts_ms, "source": "file"}
                            for c in iter_checkpoint_file(checkpoint_file, cp_size)
                            if c is not None))
                d_where, d_args = scope.where("decided_ms")
                with bw.stream("diagnostics.jsonl") as m:
                    counts["diagnostics"] = _jsonl(m, (
                        {"id": r["id"], "window_id": r["window_id"], "node_id": r["node_id"],
                         "decision": r["decision_json"], "digest": r["decision_digest"],
                         "audit_seq": r["audit_seq"]}
                        for r in conn.execute("SELECT id, window_id, node_id, decision_json, "
                                              "decision_digest, audit_seq FROM "
                                              f"diagnostics{d_where} ORDER BY id", d_args)))
                with bw.stream("feedback.csv") as m:
                    _csv(m, FEEDBACK_COLS, conn.execute(
                        f"SELECT {', '.join(FEEDBACK_COLS)} FROM feedback ORDER BY id"))
                g_where, g_args = scope.where("detected_ms")
                with bw.stream("gaps.csv") as m:
                    counts["gaps"] = _csv(m, GAP_COLS, conn.execute(
                        f"SELECT {', '.join(GAP_COLS)} FROM gaps{g_where} ORDER BY id", g_args))
                i_where, i_args = scope.where("received_ms")
                with bw.stream("ingest_log.csv") as m:
                    counts["ingest_log"] = _csv(m, INGEST_COLS, conn.execute(
                        f"SELECT {', '.join(INGEST_COLS)} FROM ingest_log{i_where} ORDER BY id",
                        i_args))
                nodes = [{"node_id": r["node_id"], "data_source": r["data_source"],
                          "first_seen_ms": r["first_seen_ms"], "last_seen_ms": r["last_seen_ms"],
                          "fw_version": r["fw_version"]}
                         for r in conn.execute("SELECT * FROM nodes ORDER BY node_id")]
                bw.put("nodes.json", json.dumps(nodes, indent=2).encode("utf-8"))
                report = verify_database(conn, hasher, checkpoint_file, checkpoint_size=cp_size)
                bw.put("verification_report.json", json.dumps(report.to_dict(), indent=2).encode())
                w_where, w_args = scope.where("received_ms")
                with bw.stream("windows.jsonl") as m:
                    counts["windows"] = _jsonl(m, (
                        {"id": r["id"], "node_id": r["node_id"], "record": r["record_json"],
                         "digest": r["record_digest"], "data_source": r["data_source"]}
                        for r in conn.execute("SELECT id, node_id, record_json, record_digest, "
                                              f"data_source FROM windows{w_where} ORDER BY id",
                                              w_args)))
            finally:
                conn.execute("COMMIT")

            sources = sorted({n["data_source"] for n in nodes})
            synthetic = any(s in ("simulator", "replay") for s in sources)
            digests = dict(sorted(bw.digests.items()))
            manifest = {
                "schema": MANIFEST_SCHEMA,
                "producer": f"algaops-gateway (engine {engine_version})",
                "gateway_id": gateway_id,
                "created_ms": created_ms,
                "hash_algorithm": "Ascon-Hash256 (NIST SP 800-232)",
                "chain": {"domain": "AlgaOps/audit/v1", "events": events, "head_seq": head_seq,
                          "head_hash": head_hash},
                "scope": {"node_id": scope.node_id, "since_ms": scope.since_ms,
                          "until_ms": scope.until_ms},
                "counts": {k: counts[k] for k in ("windows", "diagnostics", "alerts", "gaps",
                                                  "ingest_log", "checkpoints")},
                "data_sources": sources,
                "data_label": "DEMO / SYNTHETIC" if synthetic else ("LIVE HARDWARE" if sources
                                                                    else "NO DATA"),
                "policy_hash": policy_hash,
                "verification": {"ok": report.ok, "issues": len(report.issues)},
                "files": digests,
                "limitations": [
                    LIMITATION,
                    "Decisions are inspection warnings for technicians, not validated diagnoses.",
                    "Synthetic data demonstrates behaviour on designed scenarios only.",
                ],
            }
            bw.put("manifest.json", json.dumps(manifest, indent=2, ensure_ascii=False)
                   .encode("utf-8"), listed=False)
            bw.put("SHA-ASCON256SUMS", "".join(f"{d}  {n}\n" for n, d in digests.items())
                   .encode("ascii"), listed=False)
        tmp.replace(out_path)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise
    manifest["bundle_digest"] = hasher.hexdigest_file(out_path)
    return manifest
