"""ctypes binding: ABI, policy keys, engine calls."""

import math

import pytest
from gwtest import T0, make_doc

from algaops_gateway.core.engine import NodeEngine, load_policy
from algaops_gateway.core.libalgaops import OUTCOMES, CoreError
from algaops_gateway.ingest.payload_schema import parse_telemetry
import json


def window(seq=1, **over):
    d = make_doc(seq, **over)
    return parse_telemetry(json.dumps(d).encode(), d["t_end_ms"] + 50, "node-a").to_c()


def test_library_metadata(lib):
    assert lib.version == "2.0.0"
    assert lib.config_size > 0 and len(lib.config_keys) >= 60
    assert {"method.off", "method.startup", "method.steady"} <= lib.string_keys
    assert "MISSING_PACKET" not in lib.flag_list(lib.blocking_flags)
    assert "REBOOT" not in lib.flag_list(lib.blocking_flags)
    assert "STALE" in lib.flag_list(lib.blocking_flags)


def test_config_roundtrip_and_errors(lib):
    cfg = lib.new_config()
    lib.config_set(cfg, "trust.vib_max_g", 12.5)
    lib.config_set(cfg, "mode.enabled", True)
    lib.config_set(cfg, "method.steady", "mahalanobis")
    assert lib.config_get(cfg, "trust.vib_max_g") == 12.5
    assert lib.config_get(cfg, "mode.enabled") == 1
    assert lib.config_get(cfg, "method.steady") == "mahalanobis"
    with pytest.raises(CoreError):
        lib.config_set(cfg, "trust.no_such_key", 1)
    with pytest.raises(CoreError):
        lib.config_set(cfg, "method.steady", "magic")
    with pytest.raises(CoreError):
        lib.config_set(cfg, "trust.max_staleness_ms", -1)


def test_policy_hash_is_stable_and_sensitive(lib, policy, tmp_path):
    again = load_policy(lib, policy.path)
    assert again.hash == policy.hash and len(policy.hash) == 64
    text = open(policy.path, encoding="utf-8").read().replace("z_threshold = 3.0",
                                                               "z_threshold = 3.5")
    p = tmp_path / "p.toml"
    p.write_text(text, encoding="utf-8")
    assert load_policy(lib, p).hash != policy.hash


def test_policy_unknown_key_rejected(lib, tmp_path):
    p = tmp_path / "bad.toml"
    p.write_text("[trust]\nvib_max_g = 1.0\n[pump]\nrelay = true\n", encoding="utf-8")
    with pytest.raises(CoreError, match="unknown policy key"):
        load_policy(lib, p)


def test_engine_process_tick_malformed(lib, policy):
    eng = NodeEngine(lib, policy, "node-a")
    d = eng.process(window(1))
    assert d.is_trusted and OUTCOMES[d.outcome] in ("NORMAL", "NOT_EVALUATED")
    assert eng.tick(T0 + 5000) is None
    t = eng.tick(T0 + 60_000)
    assert t is not None and OUTCOMES[t.outcome] == "CHECK_SENSOR"
    assert "STREAM_TIMEOUT" in lib.flag_list(t.flags)
    m = eng.malformed(T0 + 61_000, "bad json")
    assert OUTCOMES[m.outcome] == "CHECK_SENSOR" and b"bad json" in m.reason
    eng.close()


def test_untrusted_window_never_checks_equipment(lib, policy):
    eng = NodeEngine(lib, policy, "node-a")
    for s in range(1, 30):
        eng.process(window(s))
    d = eng.process(window(30, vib_rms=float("nan")))
    assert not d.is_trusted and OUTCOMES[d.outcome] == "CHECK_SENSOR"
    assert all(d.methods[m].status == 0 for m in range(3))   # skipped
    eng.close()


def test_features_nan_mapping(lib, policy):
    eng = NodeEngine(lib, policy, "node-a")
    d = eng.process(window(1, temp_c=None))
    assert "NON_FINITE" in lib.flag_list(d.flags)
    assert not math.isnan(d.trust_score)
    eng.close()
