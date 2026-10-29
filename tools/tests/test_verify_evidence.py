"""The standalone verifier against C-CLI evidence directories (independent of the
gateway code path) and against tampered inputs."""

import shutil
import subprocess
from pathlib import Path

import pytest

import verify_evidence

ROOT = Path(__file__).resolve().parents[2]
CORE = ROOT / "algaops-assurance"


def cli():
    for n in ("algaops.exe", "algaops"):
        p = CORE / "build" / n
        if p.is_file():
            return p
    pytest.skip("C CLI not built")


def kat_vectors():
    vec, cur = [], {}
    for line in (CORE / "tests" / "vectors" / "ascon_hash256_kat.txt").read_text().splitlines():
        if " = " in line or line.endswith(" ="):
            k, _, v = line.partition("=")
            cur[k.strip()] = v.strip()
            if k.strip() == "MD":
                vec.append((bytes.fromhex(cur["Msg"]), bytes.fromhex(cur["MD"])))
    return vec


def test_independent_ascon_matches_nist_kat():
    vec = kat_vectors()
    assert len(vec) == 131
    for msg, md in vec:
        assert verify_evidence.ascon_hash256(msg) == md, msg.hex()
    for msg, md in vec[::10]:                       # the vendored reference, too
        assert verify_evidence.ascon_hash256_reference(msg) == md


def test_fast_path_equals_reference_on_random_input():
    import random
    rnd = random.Random(7)
    for n in list(range(0, 41)) + [255, 256, 257, 1000]:
        m = rnd.randbytes(n)
        assert verify_evidence.ascon_hash256(m) == verify_evidence.ascon_hash256_reference(m)


@pytest.fixture
def cli_evidence(tmp_path):
    out = tmp_path / "ev"
    r = subprocess.run([str(cli()), "--baseline", str(CORE / "data" / "baseline_normal.csv"),
                        "--input", str(CORE / "data" / "scenario_suspicious_plus_blocking.csv"),
                        "--evidence", str(out), "--data-source", "synthetic", "--quiet"],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return out


def test_independent_verifier_accepts_cli_evidence(cli_evidence):
    rep = verify_evidence.verify(cli_evidence)
    assert rep["verified"], rep["issues"]
    assert rep["events"] == 7 and rep["data_label"] == "synthetic"


def test_both_verifiers_reject_tampering(cli_evidence, tmp_path):
    bad = tmp_path / "bad"
    shutil.copytree(cli_evidence, bad)
    p = bad / "audit_events.jsonl"
    p.write_text(p.read_text(encoding="utf-8").replace("CHECK_SENSOR", "NORMAL", 1),
                 encoding="utf-8")
    rep = verify_evidence.verify(bad)
    assert not rep["verified"] and {"FILE_MODIFIED", "HASH_MISMATCH"} & {i["kind"] for i in
                                                                          rep["issues"]}
    r = subprocess.run([str(cli()), "--verify-evidence", str(bad)], capture_output=True,
                       text=True)
    assert r.returncode == 3


def test_main_exit_codes(cli_evidence, tmp_path, capsys):
    assert verify_evidence.main([str(cli_evidence)]) == 0
    assert "EVIDENCE VERIFIED" in capsys.readouterr().out
    assert verify_evidence.main([str(tmp_path / "missing")]) == 2
