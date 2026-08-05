import json

import httpx
import pytest

from htp_bridge.agent import POCKET_TERMINAL_INSTRUCTIONS, AgentClient, AgentError, FakeAgentClient
from htp_bridge.config import AgentConfig


@pytest.fixture
def agent_config():
    return AgentConfig(
        base_url="http://agent.local/v1", model="hermes-fast", api_key="", timeout_seconds=30
    )


def client_with(agent_config, handler):
    return AgentClient(agent_config, transport=httpx.MockTransport(handler))


def reply(content: str) -> httpx.Response:
    return httpx.Response(200, json={"choices": [{"message": {"content": content}}]})


async def test_converse_returns_reply_text(agent_config):
    client = client_with(agent_config, lambda request: reply("Dinner is at 7 PM."))
    assert await client.converse("what time is dinner", []) == "Dinner is at 7 PM."


async def test_converse_sends_terminal_instructions_as_system_message(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        seen["url"] = str(request.url)
        return reply("ok")

    await client_with(agent_config, handler).converse("hello", [])

    messages = seen["payload"]["messages"]
    assert seen["url"] == "http://agent.local/v1/chat/completions"
    assert messages[0]["role"] == "system"
    assert messages[0]["content"] == POCKET_TERMINAL_INSTRUCTIONS
    assert messages[-1] == {"role": "user", "content": "hello"}


async def test_converse_includes_history_in_order(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("ok")

    history = [("what time is dinner", "Dinner is at 7 PM."), ("and dessert", "Dessert is at 8 PM.")]
    await client_with(agent_config, handler).converse("thanks", history)

    roles = [m["role"] for m in seen["payload"]["messages"]]
    assert roles == ["system", "user", "assistant", "user", "assistant", "user"]
    assert seen["payload"]["messages"][1]["content"] == "what time is dinner"
    assert seen["payload"]["messages"][2]["content"] == "Dinner is at 7 PM."


async def test_converse_pins_configured_model(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("ok")

    await client_with(agent_config, handler).converse("hello", [])
    assert seen["payload"]["model"] == "hermes-fast"


async def test_converse_omits_model_when_unset(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("ok")

    config = AgentConfig(base_url=agent_config.base_url, model=None, api_key="", timeout_seconds=30)
    await AgentClient(config, transport=httpx.MockTransport(handler)).converse("hello", [])
    assert "model" not in seen["payload"]


async def test_converse_raises_on_http_failure(agent_config):
    client = client_with(agent_config, lambda request: httpx.Response(503, text="down"))
    with pytest.raises(AgentError):
        await client.converse("hello", [])


async def test_converse_raises_on_timeout(agent_config):
    def handler(request):
        raise httpx.ReadTimeout("too slow")

    with pytest.raises(AgentError):
        await client_with(agent_config, handler).converse("hello", [])


async def test_converse_raises_on_empty_reply(agent_config):
    client = client_with(agent_config, lambda request: reply("   "))
    with pytest.raises(AgentError, match="empty"):
        await client.converse("hello", [])


async def test_converse_raises_on_non_string_content(agent_config):
    client = client_with(agent_config, lambda request: httpx.Response(200, json={"choices": [{"message": {"content": None}}]}))
    with pytest.raises(AgentError, match="non-string"):
        await client.converse("hello", [])


async def test_ingest_marks_message_as_a_captured_note(agent_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["payload"] = json.loads(request.content)
        return reply("noted")

    await client_with(agent_config, handler).ingest("Add milk", recorded_at=1754300102)

    content = seen["payload"]["messages"][-1]["content"]
    assert "Add milk" in content
    assert "pocket terminal" in content.lower()


async def test_ingest_raises_on_failure_so_callers_can_retry(agent_config):
    client = client_with(agent_config, lambda request: httpx.Response(500, text="down"))
    with pytest.raises(AgentError):
        await client.ingest("Add milk", recorded_at=1)


async def test_fake_agent_records_calls_and_returns_scripted_reply():
    fake = FakeAgentClient(reply_text="Added milk to your shopping list.")

    assert await fake.converse("add milk", []) == "Added milk to your shopping list."
    await fake.ingest("a note", recorded_at=5)

    assert fake.conversations == [("add milk", [])]
    assert fake.ingested == [("a note", 5)]


async def test_fake_agent_can_raise():
    fake = FakeAgentClient(error=AgentError("agent down"))
    with pytest.raises(AgentError):
        await fake.converse("hello", [])
