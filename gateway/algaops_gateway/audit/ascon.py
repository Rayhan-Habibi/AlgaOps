"""Ascon-Hash256 (NIST SP 800-232) via the C core.

The gateway hashes with the vendored ascon-c reference code inside
libalgaops.  The standalone evidence verifier (tools/verify_evidence.py)
uses an independent vendored pure-Python reference (pyascon); tests check
both against the official KAT vectors and against each other.
"""

from __future__ import annotations

from os import PathLike

from ..core.libalgaops import AsconHash256, CoreLibrary

ZERO_HASH = bytes(32)
AUDIT_DOMAIN = b"AlgaOps/audit/v1\x00"
FILE_CHUNK = 1 << 20


class Hasher:
    def __init__(self, lib: CoreLibrary) -> None:
        self._lib = lib

    def digest(self, data: bytes) -> bytes:
        return self._lib.ascon_hash256(data)

    def hexdigest(self, data: bytes) -> str:
        return self._lib.ascon_hash256(data).hex()

    def new(self) -> AsconHash256:
        """Incremental hash (C core init/update/final), same digest as digest()."""
        return self._lib.ascon_hash256_new()

    def hexdigest_file(self, path: str | PathLike[str]) -> str:
        """Digest of a file, read in chunks of FILE_CHUNK bytes."""
        h = self.new()
        with open(path, "rb") as f:
            while chunk := f.read(FILE_CHUNK):
                h.update(chunk)
        return h.hexdigest()

    def link(self, prev: bytes, seq: int, payload: bytes) -> bytes:
        """hash_i = Ascon-Hash256(domain || 0x00 || prev || be64(seq) || payload)."""
        if seq < 1:
            raise ValueError("audit sequence numbers start at 1")
        return self._lib.audit_link(prev, seq, payload)
