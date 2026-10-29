#!/usr/bin/env python3
"""Standalone verifier for AlgaOps evidence bundles (gateway zip or C CLI directory).

Usage:  python tools/verify_evidence.py <bundle.zip | evidence-dir> [--json]

Python 3.11+ standard library only, so an auditor can run it on any machine
without the AlgaOps gateway or its C core.  It uses an INDEPENDENT Ascon
implementation (below) — not the C code that produced the bundle — and checks:

  1. every file digest in SHA-ASCON256SUMS (and manifest.json files map);
  2. the audit hash chain: contiguous seq from 1, prev-hash links, and
     hash_i = Ascon-Hash256("AlgaOps/audit/v1" 0x00 || prev || be64(i) || payload);
  3. that every exported window / decision / baseline row matches the digest
     recorded in the audit event that references it;
  4. that checkpoints match the chain (truncation below a checkpoint is detected);
  5. that every retention `archive.created` event names a chain head (the head
     of its archive bundle) that matches the chain.  Rows such an event
     archived are no longer in later bundles; the report lists the archives.

Exit status: 0 verified, 3 verification failed, 2 usage / unreadable input.

Limitation: a party able to rewrite the complete bundle including every
checkpoint can forge a consistent chain.  Compare head hashes with an earlier,
independently kept bundle or printed checkpoint.
"""

from __future__ import annotations

import argparse
import io
import json
import sys
import zipfile
from pathlib import Path

# ---------------------------------------------------------------------------
# Ascon-Hash256 (NIST SP 800-232) — vendored from the pyascon reference
# implementation by Maria Eichlseder, https://github.com/meichlseder/pyascon
# (commit ed24e54abf9507d26fa49b46a56091570c7e743e), CC0 1.0 Universal.
# Reduced to the hash variant; logic unchanged.
# ---------------------------------------------------------------------------


def _bytes_to_int(b: bytes) -> int:
    return int.from_bytes(b, "little")


def _int_to_bytes(integer: int, nbytes: int) -> bytes:
    return integer.to_bytes(nbytes, "little")


def _rotr(val: int, r: int) -> int:
    return (val >> r) | ((val & (1 << r) - 1) << (64 - r))


def _ascon_permutation(S: list[int], rounds: int = 1) -> None:
    assert rounds <= 12
    for r in range(12 - rounds, 12):
        # --- add round constants ---
        S[2] ^= (0xf0 - r * 0x10 + r * 0x1)
        # --- substitution layer ---
        S[0] ^= S[4]
        S[4] ^= S[3]
        S[2] ^= S[1]
        T = [(S[i] ^ 0xFFFFFFFFFFFFFFFF) & S[(i + 1) % 5] for i in range(5)]
        for i in range(5):
            S[i] ^= T[(i + 1) % 5]
        S[1] ^= S[0]
        S[0] ^= S[4]
        S[3] ^= S[2]
        S[2] ^= 0XFFFFFFFFFFFFFFFF
        # --- linear diffusion layer ---
        S[0] ^= _rotr(S[0], 19) ^ _rotr(S[0], 28)
        S[1] ^= _rotr(S[1], 61) ^ _rotr(S[1], 39)
        S[2] ^= _rotr(S[2], 1) ^ _rotr(S[2], 6)
        S[3] ^= _rotr(S[3], 10) ^ _rotr(S[3], 17)
        S[4] ^= _rotr(S[4], 7) ^ _rotr(S[4], 41)


def ascon_hash256_reference(message: bytes) -> bytes:
    """Ascon-Hash256 with 256-bit output (pyascon ascon_hash, variant 'Ascon-Hash256')."""
    a = b = 12  # rounds
    rate = 8    # bytes
    taglen = 256
    iv = bytes([2, 0, (b << 4) + a]) + _int_to_bytes(taglen, 2) + bytes([rate, 0, 0])
    padded_iv = iv + bytes(32)
    S = [_bytes_to_int(padded_iv[8 * w:8 * (w + 1)]) for w in range(5)]
    _ascon_permutation(S, 12)
    m_padding = bytes([0x01]) + bytes(rate - (len(message) % rate) - 1)
    m_padded = bytes(message) + m_padding
    for block in range(0, len(m_padded), rate):
        S[0] ^= _bytes_to_int(m_padded[block:block + rate])
        _ascon_permutation(S, 12)
    H = b""
    while len(H) < 32:
        H += _int_to_bytes(S[0], rate)
        _ascon_permutation(S, 12)
    return H[:32]


# Same permutation with the five state words in local variables and the
# rounds unrolled over precomputed constants (about 2x faster than the list
# version above; CPython big-int overhead bounds it near 0.35 MB/s).  tools/tests/test_verify_evidence.py checks it against the
# reference above and against the NIST known-answer vectors.

_M64 = 0xFFFFFFFFFFFFFFFF
_RC = tuple(0xf0 - r * 0x10 + r * 0x1 for r in range(12))


def _p12(x0: int, x1: int, x2: int, x3: int, x4: int) -> tuple[int, int, int, int, int]:
    m = _M64
    for c in _RC:
        x2 ^= c
        x0 ^= x4
        x4 ^= x3
        x2 ^= x1
        t0 = x0 ^ (~x1 & x2)
        t1 = x1 ^ (~x2 & x3)
        t2 = x2 ^ (~x3 & x4)
        t3 = x3 ^ (~x4 & x0)
        t4 = x4 ^ (~x0 & x1)
        t1 ^= t0
        t0 ^= t4
        t3 ^= t2
        t2 ^= m
        x0 = t0 ^ (((t0 >> 19) | (t0 << 45)) & m) ^ (((t0 >> 28) | (t0 << 36)) & m)
        x1 = t1 ^ (((t1 >> 61) | (t1 << 3)) & m) ^ (((t1 >> 39) | (t1 << 25)) & m)
        x2 = t2 ^ (((t2 >> 1) | (t2 << 63)) & m) ^ (((t2 >> 6) | (t2 << 58)) & m)
        x3 = t3 ^ (((t3 >> 10) | (t3 << 54)) & m) ^ (((t3 >> 17) | (t3 << 47)) & m)
        x4 = t4 ^ (((t4 >> 7) | (t4 << 57)) & m) ^ (((t4 >> 41) | (t4 << 23)) & m)
    return x0, x1, x2, x3, x4


def _initial_state() -> tuple[int, int, int, int, int]:
    iv = bytes([2, 0, (12 << 4) + 12]) + (256).to_bytes(2, "little") + bytes([8, 0, 0])
    return _p12(_bytes_to_int(iv), 0, 0, 0, 0)


_IV_STATE = _initial_state()


def ascon_hash256(message: bytes) -> bytes:
    """Ascon-Hash256 (NIST SP 800-232), 256-bit output."""
    x0, x1, x2, x3, x4 = _IV_STATE
    m = bytes(message) + b"" + bytes(7 - len(message) % 8)
    frombytes = int.from_bytes
    for i in range(0, len(m), 8):
        x0 ^= frombytes(m[i:i + 8], "little")
        x0, x1, x2, x3, x4 = _p12(x0, x1, x2, x3, x4)
    out = [x0.to_bytes(8, "little")]
    for _ in range(3):
        x0, x1, x2, x3, x4 = _p12(x0, x1, x2, x3, x4)
        out.append(x0.to_bytes(8, "little"))
    return b"".join(out)


# ---------------------------------------------------------------------------
# AlgaOps chain construction (docs/evidence-verification.md)
# ---------------------------------------------------------------------------

DOMAIN = b"AlgaOps/audit/v1\x00"
ZERO = bytes(32)


def chain_link(prev: bytes, seq: int, payload: bytes) -> bytes:
    return ascon_hash256(DOMAIN + prev + seq.to_bytes(8, "big") + payload)


class Bundle:
    def __init__(self, path: Path) -> None:
        self.path = path
        if path.is_dir():
            self._read = lambda name: (path / name).read_bytes()
            self._exists = lambda name: (path / name).is_file()
        else:
            zf = zipfile.ZipFile(path)
            names = set(zf.namelist())
            self._read = lambda name: zf.read(name)
            self._exists = lambda name: name in names

    def read(self, name: str) -> bytes:
        return self._read(name)

    def exists(self, name: str) -> bool:
        return self._exists(name)

    def jsonl(self, name: str) -> list[dict]:
        if not self.exists(name):
            return []
        return [json.loads(line) for line in io.StringIO(self.read(name).decode("utf-8"))
                if line.strip()]


def verify(path: Path) -> dict:
    b = Bundle(path)
    issues: list[dict] = []
    notes: list[str] = []

    def fail(kind: str, detail: str, seq: int | None = None) -> None:
        issues.append({"kind": kind, "seq": seq, "detail": detail})

    # 1. file digests
    if not b.exists("SHA-ASCON256SUMS"):
        fail("MISSING_FILE", "SHA-ASCON256SUMS not found")
        listed = {}
    else:
        listed = {}
        for line in b.read("SHA-ASCON256SUMS").decode("ascii").splitlines():
            if not line.strip():
                continue
            digest, _, name = line.partition("  ")
            listed[name] = digest
            if not b.exists(name):
                fail("MISSING_FILE", f"{name} listed but absent")
            elif ascon_hash256(b.read(name)).hex() != digest:
                fail("FILE_MODIFIED", f"{name} digest mismatch")
    manifest = None
    if b.exists("manifest.json"):
        manifest = json.loads(b.read("manifest.json"))
        for name, digest in (manifest.get("files") or {}).items():
            if isinstance(digest, str) and listed.get(name) not in (None, digest):
                fail("MANIFEST_MISMATCH", f"manifest digest for {name} differs from SUMS")
    else:
        notes.append("manifest.json not present")

    # 2. hash chain
    events = b.jsonl("audit_events.jsonl")
    prev = ZERO
    hash_at: dict[int, str] = {}
    refs = {"window": {}, "decision": {}, "baseline": {}}
    archives: list[dict] = []
    for i, ev in enumerate(events, start=1):
        seq = ev.get("seq")
        if seq != i:
            fail("SEQUENCE_GAP", f"expected seq {i}, found {seq}", seq)
        if ev.get("prev_hash") != prev.hex():
            fail("BROKEN_LINK", "prev_hash does not match the previous event", seq)
        payload = ev.get("payload", "")
        try:
            h = chain_link(bytes.fromhex(ev["prev_hash"]), int(seq), payload.encode("utf-8"))
        except (KeyError, ValueError, TypeError):
            fail("BAD_EVENT", "unreadable event line", seq)
            continue
        if h.hex() != ev.get("hash"):
            fail("HASH_MISMATCH", "event content or hash modified", seq)
        try:
            body = json.loads(payload)
        except ValueError:
            body = {}
            fail("BAD_PAYLOAD", "payload is not JSON", seq)
        if body and body.get("type") is not None and body.get("type") != ev.get("type"):
            fail("METADATA_MISMATCH", "outer type differs from hashed payload", seq)
        for key, kind in (("window", "window"), ("decision", "decision"),
                          ("baseline", "baseline")):
            rid = body.get(f"{key}_id")
            dig = body.get(f"{key}_digest") if key != "baseline" else body.get("stats_digest")
            if isinstance(rid, int) and isinstance(dig, str):
                refs[kind][rid] = (seq, dig)
        if ev.get("type") == "archive.created":
            archives.append({k: body.get(k) for k in (
                "cutoff_ms", "archived_through_seq", "file", "bundle_digest", "bundle_head_seq",
                "bundle_head_hash")} | {"seq": int(seq)})
        hash_at[int(seq)] = ev.get("hash")
        prev = bytes.fromhex(ev["hash"]) if isinstance(ev.get("hash"), str) else prev
    head_seq = len(events)
    head_hash = events[-1]["hash"] if events else ZERO.hex()

    # 3. exported rows vs referencing events
    rows_checked = 0
    for fname, kind, text_key in (("windows.jsonl", "window", "record"),
                                  ("diagnostics.jsonl", "decision", "decision"),
                                  ("baselines.jsonl", "baseline", "stats")):
        for row in b.jsonl(fname):
            rows_checked += 1
            ref = refs[kind].get(row.get("id"))
            if ref is None:
                fail("UNREFERENCED_ROW", f"{kind} {row.get('id')} has no audit event")
                continue
            seq, digest = ref
            actual = ascon_hash256(row[text_key].encode("utf-8")).hex()
            if actual != digest or row.get("digest") != digest:
                fail("ROW_MODIFIED", f"{kind} {row.get('id')} differs from audited digest", seq)
    # C CLI bundles: decisions.jsonl lines must equal the chained payloads
    if b.exists("decisions.jsonl") and not b.exists("diagnostics.jsonl"):
        lines = b.read("decisions.jsonl").decode("utf-8").splitlines()
        if [ln for ln in lines if ln] != [ev.get("payload") for ev in events]:
            fail("ROW_MODIFIED", "decisions.jsonl differs from the chained payloads")
        rows_checked += len(lines)

    # 4. checkpoints
    cps = b.jsonl("checkpoints.jsonl")
    for cp in cps:
        s = cp.get("seq")
        if not isinstance(s, int):
            continue
        if s > head_seq:
            fail("TRUNCATED", f"checkpoint at seq {s} beyond chain head {head_seq}", s)
        elif hash_at.get(s) != cp.get("hash"):
            fail("CHECKPOINT_MISMATCH", f"checkpoint hash at seq {s} differs from chain", s)
    if not cps:
        notes.append("no checkpoints in bundle: truncation of newest events not detectable")

    # 5. retention archives: each records the chain head of its archive bundle
    for a in archives:
        s, h = a["bundle_head_seq"], a["bundle_head_hash"]
        if isinstance(s, int) and isinstance(h, str) and (s >= a["seq"] or hash_at.get(s) != h):
            fail("CHECKPOINT_MISMATCH", f"archive.created records bundle head seq {s} with a "
                 "hash that differs from the chain", a["seq"])
    if archives:
        a = archives[-1]
        notes.append(f"{len(archives)} archive.created event(s): rows referenced up to seq "
                     f"{a['archived_through_seq']} were moved to archive bundles and pruned on "
                     f"the gateway (latest {a['file']}, chain head {a['bundle_head_seq']})")
    if manifest and manifest.get("chain"):
        mc = manifest["chain"]
        if mc.get("head_hash") not in (None, head_hash) or \
                mc.get("events") not in (None, head_seq):
            fail("MANIFEST_MISMATCH", "manifest chain head differs from audit_events.jsonl")

    label = (manifest or {}).get("data_label") or (manifest or {}).get("data_source")
    return {"bundle": str(path), "verified": not issues, "events": head_seq,
            "head_hash": head_hash, "rows_checked": rows_checked, "checkpoints": len(cps),
            "data_label": label, "issues": issues, "notes": notes, "archives": archives,
            "implementation": "independent pure-Python Ascon-Hash256 (pyascon-derived)"}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("bundle", type=Path)
    ap.add_argument("--json", action="store_true", help="print the full JSON report")
    args = ap.parse_args(argv)
    if not args.bundle.exists():
        print(f"error: {args.bundle} not found", file=sys.stderr)
        return 2
    try:
        rep = verify(args.bundle)
    except (zipfile.BadZipFile, OSError, ValueError) as exc:
        print(f"error: cannot read bundle: {exc}", file=sys.stderr)
        return 2
    if args.json:
        print(json.dumps(rep, indent=2))
    else:
        print(f"bundle      : {rep['bundle']}")
        print(f"data label  : {rep['data_label']}")
        print(f"chain       : {rep['events']} events, head {rep['head_hash']}")
        print(f"rows checked: {rep['rows_checked']}, checkpoints: {rep['checkpoints']}")
        for n in rep["notes"]:
            print(f"note        : {n}")
        for i in rep["issues"][:50]:
            print(f"FAIL {i['kind']:<20} seq={i['seq']} {i['detail']}")
        print("EVIDENCE VERIFIED" if rep["verified"] else "EVIDENCE VERIFICATION FAILED")
    return 0 if rep["verified"] else 3


if __name__ == "__main__":
    sys.exit(main())
