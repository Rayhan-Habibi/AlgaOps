import pytest

from algaops_sim.scenario import ScenarioError, from_dict, list_scenarios, load_scenario


def test_all_shipped_scenarios_load():
    names = {s.name for s in list_scenarios()}
    expected = {"baseline_modes", "normal_steady", "normal_8h_replay", "mode_transitions",
                "stuck_vibration", "stuck_temperature", "missing_packet", "duplicate_sequence",
                "out_of_order_sequence", "bad_timestamp", "stale_late_delivery",
                "out_of_range_spike", "non_finite_values", "malformed_payload",
                "self_test_failure", "sensor_disagreement", "equipment_vibration_anomaly",
                "equipment_thermal_anomaly", "suspicious_plus_blocking",
                "disconnect_reconnect_replay"}
    assert expected <= names


def test_yaml_on_off_booleans_are_schedule_states():
    sc = load_scenario("baseline_modes")
    assert sc.running_at(0) is False and sc.running_at(60) is True and sc.running_at(200) is False


@pytest.mark.parametrize("doc,msg", [
    ({"name": "x"}, "schema"),
    ({"schema": "algaops.scenario.v1", "name": "x", "bogus": 1}, "unknown key"),
    ({"schema": "algaops.scenario.v1", "name": "x", "sensor_faults": [{"type": "melt"}]},
     "unknown type"),
    ({"schema": "algaops.scenario.v1", "name": "x", "duration_windows": 0}, "out of range"),
    ({"schema": "algaops.scenario.v1", "name": "x", "schedule": [{"at": 0, "state": "maybe"}]},
     "on/off"),
    ({"schema": "algaops.scenario.v1", "name": "x", "asset": {"warp": 9}}, "asset"),
])
def test_invalid_scenarios(doc, msg):
    with pytest.raises(ScenarioError, match=msg):
        from_dict(doc, "test")
