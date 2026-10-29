import hashlib

import pytest

from algaops_gateway.sources.pbr_readonly_csv import PbrReadonlyCsvSource


def test_requires_permission(tmp_path):
    with pytest.raises(PermissionError):
        PbrReadonlyCsvSource(tmp_path / "x.csv", permission_confirmed=False)


def test_tails_rows_without_modifying_the_file(tmp_path):
    p = tmp_path / "pbr.csv"
    p.write_text("timestamp,light_umol,co2_ppm,ph\n1,850,410,7.1\n2,860,nan,7.2\n",
                 encoding="utf-8")
    src = PbrReadonlyCsvSource(p, permission_confirmed=True)
    digest = hashlib.sha256(p.read_bytes()).hexdigest()
    assert src.poll() == 2
    assert hashlib.sha256(p.read_bytes()).hexdigest() == digest, "source file must not change"
    snap = src.snapshot()
    assert snap["columns"] == ["timestamp", "light_umol", "co2_ppm", "ph"]
    assert snap["latest"][-1]["co2_ppm"] is None and snap["used_for_diagnosis"] is False
    with open(p, "a", encoding="utf-8") as fh:
        fh.write("3,870,415,7.0\n4,8")       # partial last line is not read yet
    assert src.poll() == 1
    with open(p, "a", encoding="utf-8") as fh:
        fh.write("80,416,7.0\n")
    assert src.poll() == 1 and src.snapshot()["latest"][-1]["light_umol"] == 880.0


def test_rotation_restarts(tmp_path):
    p = tmp_path / "pbr.csv"
    p.write_text("a,b\n1,2\n3,4\n", encoding="utf-8")
    src = PbrReadonlyCsvSource(p, permission_confirmed=True)
    assert src.poll() == 2
    p.write_text("a,b\n5,6\n", encoding="utf-8")
    assert src.poll() == 1
