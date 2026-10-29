"""One algorithm, many paths: the gateway (Python payload parsing -> ctypes) and
the C CLI (C CSV reader) must reach identical decisions on the same inputs."""

import csv
import json
import shutil
import subprocess
from pathlib import Path

import pytest
from algaops_sim.csv_export import export_csv
from algaops_sim.replay import generate_stream
from algaops_sim.scenario import load_scenario

from algaops_gateway.analytics.evaluate import run_scenario
from algaops_gateway.config import DEFAULT_POLICY, REPO_ROOT
from algaops_gateway.core.engine import load_policy, replay_csv
from algaops_gateway.core.libalgaops import OUTCOMES

CORE = REPO_ROOT / "algaops-assurance"
GOLDEN = CORE / "tests" / "golden" / "scenario_outcomes.csv"
LETTER = {"NORMAL": "N", "CHECK_SENSOR": "S", "CHECK_EQUIPMENT": "E", "NOT_EVALUATED": "X"}


def golden_rows():
    lines = [ln for ln in GOLDEN.read_text(encoding="utf-8").splitlines()
             if ln and not ln.startswith("#")]
    return list(csv.DictReader(lines))


@pytest.mark.parametrize("row", golden_rows(), ids=lambda r: Path(r["scenario"]).stem)
def test_golden_outcomes_through_binding(lib, row):
    policy = load_policy(lib, DEFAULT_POLICY if row["profile"] == "policy" else None)
    baseline = CORE / row["baseline"] if row["baseline"] else None
    rows = replay_csv(lib, policy, CORE / row["scenario"], baseline)
    got = "".join(LETTER[OUTCOMES[d.outcome]] for _, d, _ in rows)
    assert got == row["expected"]
    flags = 0
    for _, d, _ in rows:
        flags |= d.flags
    for name in filter(None, row["required_flags"].split("|")):
        assert name in lib.flag_list(flags)


def cli_path() -> Path | None:
    for name in ("algaops.exe", "algaops"):
        p = CORE / "build" / name
        if p.is_file():
            return p
    found = shutil.which("algaops")
    return Path(found) if found else None


@pytest.mark.parametrize("name", ["stuck_vibration", "sensor_disagreement",
                                  "equipment_vibration_anomaly", "duplicate_sequence",
                                  "malformed_payload", "out_of_order_sequence",
                                  "stale_late_delivery", "mode_transitions"])
def test_gateway_pipeline_matches_c_cli(lib, tmp_path, name):
    cli = cli_path()
    if cli is None:
        pytest.skip("C CLI not built")
    policy = load_policy(lib, DEFAULT_POLICY)
    # gateway path: payload JSON -> validation -> ctypes engine (no baselines => compare
    # with the CLI run that also has none except the commissioning CSV below)
    base_csv = tmp_path / "baseline.csv"
    events, _ = generate_stream(load_scenario("baseline_modes"))
    export_csv(events, base_csv, "baseline_modes", "commissioning")
    from algaops_gateway.core.engine import baselines_from_csv
    bls = baselines_from_csv(lib, policy, base_csv)
    gw = run_scenario(lib, policy, name, bls, keep_timeline=True)
    gw_outcomes = [t["outcome"] for t in gw["timeline"] if t["outcome"] != "DROPPED"]

    # C CLI path: the same received stream exported as CSV
    events, _ = generate_stream(load_scenario(name))
    inp = tmp_path / f"{name}.csv"
    export_csv(events, inp, name, "parity")
    out = tmp_path / "decisions.jsonl"
    res = subprocess.run([str(cli), "--config", str(DEFAULT_POLICY), "--baseline", str(base_csv),
                          "--input", str(inp), "--json", str(out), "--quiet"],
                         capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stderr
    cli_outcomes = [json.loads(ln)["outcome"] for ln in out.read_text().splitlines()]
    assert cli_outcomes == gw_outcomes
