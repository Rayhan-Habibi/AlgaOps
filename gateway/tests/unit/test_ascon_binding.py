"""Ascon-Hash256 through the C binding vs official KATs and the independent
pure-Python reference used by tools/verify_evidence.py."""

import random

import pytest
from verify_evidence import ascon_hash256 as py_ascon
from verify_evidence import chain_link as py_link

from algaops_gateway.audit.ascon import ZERO_HASH, Hasher
from algaops_gateway.config import REPO_ROOT

KAT = REPO_ROOT / "algaops-assurance" / "tests" / "vectors" / "ascon_hash256_kat.txt"


def load_kats():
    entries, cur = [], {}
    for line in KAT.read_text(encoding="ascii").splitlines():
        if line.startswith("#"):
            continue
        if not line.strip():
            if cur:
                entries.append(cur)
                cur = {}
            continue
        k, _, v = line.partition(" = ")
        cur[k] = v
    if cur:
        entries.append(cur)
    return entries


def test_kat_vectors_c_binding(lib):
    kats = load_kats()
    assert len(kats) >= 131
    for e in kats:
        assert lib.ascon_hash256(bytes.fromhex(e["Msg"])).hex().upper() == e["MD"], e["Count"]


@pytest.mark.parametrize("count", [1, 2, 9, 17, 64, 130, 1025])
def test_kat_vectors_independent_python(count):
    e = next(k for k in load_kats() if int(k["Count"]) == count)
    assert py_ascon(bytes.fromhex(e["Msg"])).hex().upper() == e["MD"]


def test_kat_vectors_incremental_every_split(lib):
    """init/update/final of the C core: every two-way split and byte by byte."""
    for e in load_kats():
        msg = bytes.fromhex(e["Msg"])
        for cut in range(len(msg) + 1):
            h = lib.ascon_hash256_new()
            h.update(msg[:cut])
            h.update(msg[cut:])
            assert h.hexdigest().upper() == e["MD"], (e["Count"], cut)
        h = lib.ascon_hash256_new()
        for i in range(len(msg)):
            h.update(msg[i:i + 1])
        assert h.hexdigest().upper() == e["MD"], e["Count"]


def test_incremental_equals_one_shot_on_random_pieces(lib):
    rng = random.Random(1)
    for n in [0, 1, 7, 8, 9, 63, 64, 65, 1000, 100_003, 3_000_001]:
        msg = rng.randbytes(n)
        h = lib.ascon_hash256_new()
        i = 0
        while i < n:
            k = rng.choice([0, 1, 3, 8, 13, 4096, 65_537, 1 << 20])
            h.update(msg[i:i + k])
            i += k
        assert h.digest() == lib.ascon_hash256(msg), n
    for n in [5, 300]:                           # and the independent implementation
        msg = rng.randbytes(n)
        h = lib.ascon_hash256_new()
        h.update(msg[:n // 3])
        h.update(msg[n // 3:])
        assert h.digest() == py_ascon(msg)


def test_incremental_hash_object(lib):
    h = lib.ascon_hash256_new()
    h.update(bytearray(b"abc"))
    h.update(memoryview(b"def"))
    d = h.digest()
    assert d == lib.ascon_hash256(b"abcdef") and h.digest() == d and h.hexdigest() == d.hex()
    with pytest.raises(ValueError):
        h.update(b"g")                           # the C context is wiped by final()


def test_hasher_digests_files_in_chunks(lib, tmp_path):
    data = random.Random(3).randbytes(2 * (1 << 20) + 12_345)    # three chunks
    p = tmp_path / "data.bin"
    p.write_bytes(data)
    h = Hasher(lib)
    assert h.hexdigest_file(p) == h.hexdigest(data)
    empty = tmp_path / "empty.bin"
    empty.write_bytes(b"")
    assert h.hexdigest_file(empty) == h.hexdigest(b"")


def test_c_and_python_agree_on_random_messages(lib):
    rng = random.Random(42)
    for n in [0, 1, 7, 8, 9, 15, 16, 31, 33, 255, 1000]:
        msg = bytes(rng.getrandbits(8) for _ in range(n))
        assert lib.ascon_hash256(msg) == py_ascon(msg)


def test_chain_link_matches_independent_implementation(lib):
    h = Hasher(lib)
    prev = ZERO_HASH
    for seq in range(1, 6):
        payload = f'{{"seq":{seq},"type":"window.diagnosed"}}'.encode()
        c = h.link(prev, seq, payload)
        assert c == py_link(prev, seq, payload)
        prev = c


def test_link_rejects_seq_zero(lib):
    with pytest.raises(ValueError):
        Hasher(lib).link(ZERO_HASH, 0, b"{}")
