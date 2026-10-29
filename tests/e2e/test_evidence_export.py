"""Evidence export through the HTTP API, verified on its own by the standalone
tool (tools/verify_evidence.py, pure Python, independent Ascon code)."""

import json
import subprocess
import sys
import zipfile

from e2e_support import ROOT, TOKEN

AUTH = {"Authorization": f"Bearer {TOKEN}"}
VERIFIER = ROOT / "tools" / "verify_evidence.py"


def export(system, tmp_path, **scope):
    r = system.http.post("/api/export/evidence", headers=AUTH, json=scope)
    assert r.status_code == 200, r.text
    body = r.json()
    z = system.http.get(body["url"])
    assert z.status_code == 200
    path = tmp_path / body["file"]
    path.write_bytes(z.content)
    return body["manifest"], path


def standalone(path):
    return subprocess.run([sys.executable, "-I", str(VERIFIER), str(path), "--json"],
                          capture_output=True, text=True, timeout=900)


def test_export_requires_the_api_token(system):
    assert system.http.post("/api/export/evidence", json={}).status_code == 401
    assert system.http.post("/api/export/evidence", json={},
                            headers={"Authorization": "Bearer wrong-token-0000000"}
                            ).status_code == 401


def test_full_bundle_verifies_independently(system, tmp_path):
    man, path = export(system, tmp_path)
    assert man["data_label"] == "DEMO / SYNTHETIC" and man["verification"]["ok"]
    with system.db() as c:
        n_windows = c.execute("SELECT COUNT(*) FROM windows").fetchone()[0]
    assert man["counts"]["windows"] == n_windows
    out = standalone(path)
    assert out.returncode == 0, out.stdout[-2000:] + out.stderr[-2000:]
    rep = json.loads(out.stdout)
    assert rep["verified"] and rep["events"] >= man["chain"]["head_seq"]
    assert rep["data_label"] == "DEMO / SYNTHETIC"


def test_node_scoped_bundle_holds_only_that_node(system, tmp_path):
    man, path = export(system, tmp_path, node_id="e2e-equipment")
    with zipfile.ZipFile(path) as zf:
        rows = [json.loads(line) for line in zf.read("windows.jsonl").decode().splitlines()
                if line.strip()]
    assert rows and {r["node_id"] for r in rows} == {"e2e-equipment"}
    assert standalone(path).returncode == 0


def test_edited_bundle_is_rejected(system, tmp_path):
    _, path = export(system, tmp_path, node_id="e2e-stuck")
    forged = tmp_path / "forged.zip"
    with zipfile.ZipFile(path) as src, zipfile.ZipFile(forged, "w", zipfile.ZIP_DEFLATED) as dst:
        for name in src.namelist():
            data = src.read(name)
            if name == "diagnostics.jsonl":
                rows = [json.loads(line) for line in data.decode().splitlines() if line.strip()]
                row = next(r for r in rows if '"outcome":"CHECK_SENSOR"' in r["decision"])
                row["decision"] = row["decision"].replace('"outcome":"CHECK_SENSOR"',
                                                          '"outcome":"NORMAL"')
                data = "".join(json.dumps(r, separators=(",", ":")) + "\n" for r in rows).encode()
            dst.writestr(name, data)
    out = standalone(forged)
    assert out.returncode == 3, out.stdout[-1500:]
    assert not json.loads(out.stdout)["verified"]
