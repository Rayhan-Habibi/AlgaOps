import pytest

from algaops_gateway.config import ConfigError, is_loopback, load_config


def write(tmp_path, text):
    p = tmp_path / "gw.toml"
    p.write_text(text, encoding="utf-8")
    return p


def test_defaults_are_local_only():
    cfg = load_config(None, env={})
    assert cfg.api.host == "127.0.0.1" and cfg.mqtt.host == "127.0.0.1"
    assert cfg.api.token_file is None


def test_lan_binding_requires_token(tmp_path):
    with pytest.raises(ConfigError, match="token_file"):
        load_config(write(tmp_path, '[api]\nhost = "192.168.1.10"\n'), env={})
    tok = tmp_path / "token"
    tok.write_text("0123456789abcdef0123", encoding="utf-8")
    cfg = load_config(write(tmp_path, f'[api]\nhost = "192.168.1.10"\ntoken_file = "{tok.name}"\n'),
                      env={})
    assert cfg.api_token() == "0123456789abcdef0123"
    cfg = load_config(write(tmp_path, '[api]\nhost = "0.0.0.0"\nallow_insecure_lan = true\n'),
                      env={})
    assert cfg.api.allow_insecure_lan


def test_short_token_rejected(tmp_path):
    tok = tmp_path / "token"
    tok.write_text("short", encoding="utf-8")
    cfg = load_config(write(tmp_path, f'[api]\ntoken_file = "{tok.name}"\n'), env={})
    with pytest.raises(ConfigError):
        cfg.api_token()


def test_unknown_keys_and_sections_rejected(tmp_path):
    with pytest.raises(ConfigError, match="unknown"):
        load_config(write(tmp_path, "[mqtt]\nhots = 'x'\n"), env={})
    with pytest.raises(ConfigError, match="section"):
        load_config(write(tmp_path, "[pump]\nstart = true\n"), env={})


def test_pbr_requires_permission(tmp_path):
    with pytest.raises(ConfigError, match="permission"):
        load_config(write(tmp_path, "[pbr]\nenabled = true\npath = 'x.csv'\n"), env={})


def test_env_overrides(tmp_path):
    cfg = load_config(None, env={"ALGAOPS_MQTT_PORT": "18830", "ALGAOPS_DB": "x/y.db"})
    assert cfg.mqtt.port == 18830 and cfg.storage.db_path == "x/y.db"
    with pytest.raises(ConfigError):
        load_config(None, env={"ALGAOPS_MQTT_PORT": "notaport"})


def test_retention_days(tmp_path):
    assert load_config(None, env={}).storage.retention_days == 0, "default keeps everything"
    cfg = load_config(write(tmp_path, "[storage]\nretention_days = 30\n"), env={})
    assert cfg.storage.retention_days == 30
    for bad in ("-1", "1.5", '"30"', "true"):
        with pytest.raises(ConfigError, match="retention_days"):
            load_config(write(tmp_path, f"[storage]\nretention_days = {bad}\n"), env={})


def test_relative_paths_resolve_against_config_dir(tmp_path):
    cfg = load_config(write(tmp_path, "[storage]\ndb_path = 'data/a.db'\n"), env={})
    assert cfg.resolve(cfg.storage.db_path) == (tmp_path / "data" / "a.db").resolve()


def test_is_loopback():
    assert is_loopback("127.0.0.1") and is_loopback("::1") and is_loopback("localhost")
    assert not is_loopback("0.0.0.0") and not is_loopback("10.0.0.2")


def test_config_saved_with_a_utf8_bom_loads(tmp_path):
    p = tmp_path / "gateway.toml"
    p.write_bytes(b"\xef\xbb\xbfgateway_id = \"gw-bom\"\n[api]\nport = 8099\n")
    cfg = load_config(p, env={})
    assert cfg.gateway_id == "gw-bom" and cfg.api.port == 8099
