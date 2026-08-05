import pytest

from htp_bridge.config import ConfigError, load_config

VALID = """
[server]
host = "127.0.0.1"
port = 8787
max_upload_bytes = 4200000
sync_interval_seconds = 600

[[devices]]
id = "pocket-01"
token = "tok-aaaaaaaaaaaaaaaaaaaa"

[salutations]
prefixes = ["hey hermes", "hermes"]

[agent]
base_url = "http://127.0.0.1:8080/v1"
model = "hermes-fast"
api_key = ""
timeout_seconds = 60

[speech]
provider = "openai"
api_key = "sk-test"
stt_model = "whisper-1"
tts_model = "tts-1"
tts_voice = "alloy"
timeout_seconds = 60

[storage]
db_path = "/tmp/htp.db"
audio_dir = "/tmp/htp-audio"
upload_retention_days = 365
reply_retention_days = 7

[dashboard]
max_items = 32
max_text_chars = 40
"""


def write(tmp_path, text):
    path = tmp_path / "config.toml"
    path.write_text(text)
    return path


def test_loads_valid_config(tmp_path):
    cfg = load_config(write(tmp_path, VALID))
    assert cfg.server.port == 8787
    assert cfg.devices[0].id == "pocket-01"
    assert cfg.salutations.prefixes == ["hey hermes", "hermes"]
    assert cfg.agent.model == "hermes-fast"
    assert cfg.dashboard.max_items == 32


def test_rejects_config_with_no_devices(tmp_path):
    text = VALID.replace('[[devices]]\nid = "pocket-01"\ntoken = "tok-aaaaaaaaaaaaaaaaaaaa"\n', "")
    with pytest.raises(ConfigError, match="at least one device"):
        load_config(write(tmp_path, text))


def test_rejects_short_token(tmp_path):
    text = VALID.replace("tok-aaaaaaaaaaaaaaaaaaaa", "short")
    with pytest.raises(ConfigError, match="at least 16 characters"):
        load_config(write(tmp_path, text))


def test_rejects_duplicate_device_ids(tmp_path):
    text = VALID + '\n[[devices]]\nid = "pocket-01"\ntoken = "tok-bbbbbbbbbbbbbbbbbbbb"\n'
    with pytest.raises(ConfigError, match="duplicate device id"):
        load_config(write(tmp_path, text))


def test_rejects_empty_salutation_list(tmp_path):
    text = VALID.replace('prefixes = ["hey hermes", "hermes"]', "prefixes = []")
    with pytest.raises(ConfigError, match="at least one salutation"):
        load_config(write(tmp_path, text))


def test_missing_section_names_the_section(tmp_path):
    text = VALID.replace("[agent]", "[agent-disabled]")
    with pytest.raises(ConfigError, match="agent"):
        load_config(write(tmp_path, text))
