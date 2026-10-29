import csv

from algaops_sim.csv_export import COLUMNS, export_csv
from algaops_sim.replay import generate_stream
from algaops_sim.scenario import load_scenario


def rows(path):
    lines = [ln for ln in path.read_text(encoding="utf-8").splitlines() if not ln.startswith("#")]
    return list(csv.DictReader(lines))


def test_export_columns_and_synthetic_header(tmp_path):
    sc = load_scenario("non_finite_values")
    events, _ = generate_stream(sc)
    p = tmp_path / "x.csv"
    n = export_csv(events, p, sc.name, sc.description)
    text = p.read_text(encoding="utf-8")
    assert "SYNTHETIC DATA" in text and n == sc.duration_windows
    r = rows(p)
    assert list(r[0]) == COLUMNS
    assert r[80]["vibration_rms_g"] == "nan" and r[100]["temperature_c"] == "nan"
    assert r[120]["current_a"] == "nan" and r[0]["self_test"] == "pass"


def test_redeliveries_dropped_and_malformed_rows(tmp_path):
    for name, check in [("duplicate_sequence", lambda r: len(r) == 201),
                        ("malformed_payload", lambda r: sum(x["timestamp_ms"] == "MALFORMED"
                                                            for x in r) == 5)]:
        sc = load_scenario(name)
        events, _ = generate_stream(sc)
        p = tmp_path / f"{name}.csv"
        export_csv(events, p, sc.name, sc.description)
        assert check(rows(p)), name
