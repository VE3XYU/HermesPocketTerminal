import pytest
from fastapi.testclient import TestClient

from htp_bridge.config import load_config
from htp_bridge.main import build_deps, create_full_app, main

CONFIG_TEMPLATE = """
[server]
host = "127.0.0.1"
port = 8787
max_upload_bytes = 4200000
sync_interval_seconds = 600

[[devices]]
id = "pocket-01"
token = "tok-aaaaaaaaaaaaaaaaaaaa"

[salutations]
prefixes = ["hey hermes"]

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
db_path = "{db}"
audio_dir = "{audio}"
upload_retention_days = 365
reply_retention_days = 7

[dashboard]
max_items = 32
max_text_chars = 40
"""


@pytest.fixture
def config_path(tmp_path):
    path = tmp_path / "config.toml"
    path.write_text(
        CONFIG_TEMPLATE.format(db=tmp_path / "htp.db", audio=tmp_path / "audio")
    )
    return path


def test_build_deps_wires_every_component(config_path):
    deps, tools = build_deps(load_config(config_path))
    assert deps.devices.authenticate("tok-aaaaaaaaaaaaaaaaaaaa").id == "pocket-01"
    assert tools.dashboard is deps.dashboard
    assert tools.notifications is deps.notifications


def test_full_app_serves_healthz_and_requires_auth_on_htp(config_path):
    app = create_full_app(load_config(config_path))
    with TestClient(app) as client:  # the context manager runs the lifespan
        assert client.get("/healthz").status_code == 200
        assert client.get("/htp/v1/dashboard").status_code == 401


def test_full_app_mounts_the_mcp_endpoint(config_path):
    app = create_full_app(load_config(config_path))
    assert any(getattr(route, "path", "").startswith("/mcp") for route in app.routes)


def test_mcp_endpoint_answers_initialize_at_the_documented_url(config_path):
    """The README tells Hermes Agent to use <base>/mcp -- that URL must work.

    A route existing under /mcp is not enough: FastMCP's own app serves at its
    configured streamable_http_path, so mounting it without pinning that path
    puts the real endpoint one level deeper and every request 404s.
    """
    app = create_full_app(load_config(config_path))
    # base_url pins the Host header: the MCP SDK enables DNS-rebinding
    # protection by default and only allows 127.0.0.1/localhost/[::1], which is
    # how Hermes Agent reaches this mount. TestClient's default "testserver"
    # host would be rejected with 421 before routing is ever reached.
    with TestClient(app, base_url="http://127.0.0.1:8787") as client:
        response = client.post(
            "/mcp",
            json={
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "test-client", "version": "1.0"},
                },
            },
            headers={"Accept": "application/json, text/event-stream"},
        )
    assert response.status_code == 200
    assert "serverInfo" in response.text


def test_main_reports_missing_config_without_traceback(tmp_path, capsys):
    exit_code = main(["--config", str(tmp_path / "absent.toml")])
    assert exit_code == 1
    assert "not found" in capsys.readouterr().err


def test_build_deps_gives_the_pipeline_a_real_timing_store(config_path):
    from htp_bridge.timing import NullTimingStore, TimingStore

    deps, _ = build_deps(load_config(config_path))
    assert isinstance(deps.pipeline._timings, TimingStore)
    assert not isinstance(deps.pipeline._timings, NullTimingStore)
