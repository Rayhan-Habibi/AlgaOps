"""Evidence bundle: contents, independent verification, tamper detection, memory."""

import json
import sqlite3
import tracemalloc
import zipfile

import verify_evidence
from gwtest import feed

from algaops_gateway.audit import ascon
from algaops_gateway.audit.checkpoint import read_checkpoint_file
from algaops_gateway.export import evidence_package
from algaops_gateway.export.evidence_package import ExportScope, build_bundle


def make_bundle(pipeline, clock, tmp_path, n=40, **scope):
    feed(pipeline, clock, n)
    feed(pipeline, clock, 3, start_seq=n + 1, samples_valid=10)
    pipeline._maybe_checkpoint(force=True)
    out = tmp_path / "bundle.zip"
    man = build_bundle(pipeline.db.connect_readonly(), pipeline.hasher, out, gateway_id="test-gw",
                       engine_version="2.0.0", policy_hash=pipeline.policy.hash, created_ms=1,
                       scope=ExportScope(**scope), checkpoint_file=tmp_path / "checkpoints.jsonl")
    return out, man


def test_bundle_contents_and_manifest(pipeline, clock, tmp_path):
    out, man = make_bundle(pipeline, clock, tmp_path)
    names = set(zipfile.ZipFile(out).namelist())
    for n in ("manifest.json", "SHA-ASCON256SUMS", "audit_events.jsonl", "windows.jsonl",
              "diagnostics.jsonl", "alerts.csv", "feedback.csv", "checkpoints.jsonl",
              "verification_report.json", "README.txt", "baselines.jsonl", "nodes.json"):
        assert n in names
    assert man["data_label"] == "DEMO / SYNTHETIC" and man["verification"]["ok"]
    assert man["counts"]["windows"] == 43 and man["chain"]["head_seq"] > 43
    blob = b"".join(zipfile.ZipFile(out).read(n) for n in names)
    assert b"password" not in blob.lower() and b"token" not in blob.lower()


def test_independent_verifier_accepts_bundle(pipeline, clock, tmp_path):
    out, man = make_bundle(pipeline, clock, tmp_path)
    rep = verify_evidence.verify(out)
    assert rep["verified"], rep["issues"]
    assert rep["events"] == man["chain"]["head_seq"] and rep["head_hash"] == man["chain"]["head_hash"]


def _rewrite(src, dst, name, fn):
    with zipfile.ZipFile(src) as zi, zipfile.ZipFile(dst, "w") as zo:
        for n in zi.namelist():
            data = zi.read(n)
            zo.writestr(n, fn(data) if n == name else data)


def test_verifier_detects_tampering(pipeline, clock, tmp_path):
    out, _ = make_bundle(pipeline, clock, tmp_path)

    def flip_outcome(data):
        return data.replace(b'CHECK_SENSOR', b'NORMAL', 1)

    def drop_event(data):
        lines = data.splitlines(keepends=True)
        return b"".join(lines[:5] + lines[6:])

    def edit_window(data):
        lines = data.splitlines(keepends=True)
        row = json.loads(lines[3])
        row["record"] = row["record"].replace('"temp_c":3', '"temp_c":9')
        lines[3] = (json.dumps(row) + "\n").encode()
        return b"".join(lines)

    cases = [("audit_events.jsonl", flip_outcome, {"FILE_MODIFIED", "HASH_MISMATCH"}),
             ("audit_events.jsonl", drop_event, {"FILE_MODIFIED", "SEQUENCE_GAP", "BROKEN_LINK"}),
             ("windows.jsonl", edit_window, {"FILE_MODIFIED", "ROW_MODIFIED"})]
    for i, (name, fn, kinds) in enumerate(cases):
        bad = tmp_path / f"bad{i}.zip"
        _rewrite(out, bad, name, fn)
        rep = verify_evidence.verify(bad)
        assert not rep["verified"]
        assert kinds & {x["kind"] for x in rep["issues"]}, (name, rep["issues"][:3])


def test_scope_filters_rows_but_keeps_chain(pipeline, clock, tmp_path):
    out, man = make_bundle(pipeline, clock, tmp_path, node_id="no-such-node")
    assert man["counts"]["windows"] == 0 and man["chain"]["head_seq"] > 0
    assert verify_evidence.verify(out)["verified"]


def test_memory_does_not_grow_with_the_chain_or_the_rows(pipeline, clock, tmp_path,
                                                          monkeypatch):
    """Members are streamed and verification merges the chain with the tables: three
    times the windows leave the peak where it was.  Holding the chain and the rows in
    memory took about 15 KB per window (14 MB more for the second bundle), the
    verification's references alone about 0.6 KB per event (0.6 MB more)."""
    monkeypatch.setattr(evidence_package, "CHUNK", 4096)       # buffers out of the way
    monkeypatch.setattr(ascon, "FILE_CHUNK", 4096)
    peaks = []
    for start, n in ((1, 500), (501, 1000)):
        feed(pipeline, clock, n, start_seq=start)
        conn = pipeline.db.connect_readonly()
        tracemalloc.start()
        try:
            man = build_bundle(conn, pipeline.hasher, tmp_path / f"b{start}.zip",
                               gateway_id="test-gw", engine_version="2.0.0",
                               policy_hash=pipeline.policy.hash, created_ms=1,
                               checkpoint_file=tmp_path / "checkpoints.jsonl")
            peaks.append(tracemalloc.get_traced_memory()[1])
        finally:
            tracemalloc.stop()
            conn.close()
        assert man["verification"]["ok"] and man["counts"]["windows"] == start + n - 1
    assert peaks[1] < peaks[0] + 256_000, peaks


class _GatewayGoesOn(sqlite3.Connection):
    """Read connection that lets the gateway write on once its snapshot has begun."""
    then = None

    def execute(self, sql, *args):
        cur = super().execute(sql, *args)    # a SELECT is stepped once: the snapshot begins
        if self.then is not None and sql.lstrip().upper().startswith("SELECT"):
            then, self.then = self.then, None
            then()
        return cur


def test_checkpoints_appended_during_the_export_are_left_out(pipeline, clock, tmp_path):
    """The external file is read up to its size when the snapshot began: a checkpoint
    appended meanwhile names an event the bundle does not hold (it would read as
    truncation, and a retention archive would be discarded)."""
    feed(pipeline, clock, 40)
    pipeline._maybe_checkpoint(force=True)
    cp_file = tmp_path / "checkpoints.jsonl"
    conn = sqlite3.connect(f"{pipeline.db.path.resolve().as_uri()}?mode=ro", uri=True,
                           isolation_level=None, factory=_GatewayGoesOn)
    conn.row_factory = sqlite3.Row

    def gateway_goes_on():
        feed(pipeline, clock, 60, start_seq=41)
        pipeline._maybe_checkpoint(force=True)
    conn.then = gateway_goes_on
    try:
        man = build_bundle(conn, pipeline.hasher, tmp_path / "b.zip", gateway_id="test-gw",
                           engine_version="2.0.0", policy_hash=pipeline.policy.hash,
                           created_ms=1, checkpoint_file=cp_file)
    finally:
        conn.close()
    late = read_checkpoint_file(cp_file).checkpoints[-1]
    assert late.seq == pipeline.chain.head[0] > man["chain"]["head_seq"], "the gateway went on"
    assert man["verification"]["ok"]
    rep = verify_evidence.verify(tmp_path / "b.zip")
    assert rep["verified"], rep["issues"][:3]
    with zipfile.ZipFile(tmp_path / "b.zip") as z:
        seqs = [json.loads(line)["seq"] for line in z.read("checkpoints.jsonl").splitlines()]
    assert seqs and max(seqs) == man["chain"]["head_seq"]
