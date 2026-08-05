from __future__ import annotations

from typing import Any

import httpx

from htp_bridge.config import AgentConfig

POCKET_TERMINAL_INSTRUCTIONS = (
    "You are replying to a message captured on a pocket terminal and read aloud by "
    "text-to-speech. Reply in one or two short plain-text sentences. Use no markdown, "
    "no lists, and no preamble. Refer to times absolutely (for example \"at 10:00 AM\"), "
    "never relatively (\"in 15 minutes\"), because delivery may be delayed."
)


class AgentError(Exception):
    """Raised when Hermes Agent is unreachable or returns an unusable response."""


class AgentClient:
    def __init__(self, config: AgentConfig, transport: httpx.BaseTransport | None = None) -> None:
        self._config = config
        self._transport = transport

    async def converse(self, text: str, history: list[tuple[str, str]]) -> str:
        messages: list[dict[str, str]] = [
            {"role": "system", "content": POCKET_TERMINAL_INSTRUCTIONS}
        ]
        for user_text, assistant_text in history:
            messages.append({"role": "user", "content": user_text})
            messages.append({"role": "assistant", "content": assistant_text})
        messages.append({"role": "user", "content": text})

        data = await self._post(messages)
        try:
            raw_content = data["choices"][0]["message"]["content"]
            if not isinstance(raw_content, str):
                raise AgentError(f"agent returned non-string content: {type(raw_content).__name__}")
            content = raw_content.strip()
        except (KeyError, IndexError, TypeError) as exc:
            raise AgentError(f"unexpected agent response shape: {exc}") from exc
        if not content:
            raise AgentError("agent returned an empty reply")
        return content

    async def ingest(self, text: str, recorded_at: int | None) -> None:
        """Deliver a note to the agent for classification and storage.

        Raises on failure so the caller can retry; the device is not waiting.
        """
        stamp = f" at epoch {recorded_at}" if recorded_at else ""
        messages = [
            {
                "role": "user",
                "content": (
                    f"The following was captured as a note on the pocket terminal{stamp}. "
                    f"Handle it according to its content; no spoken reply is expected.\n\n{text}"
                ),
            }
        ]
        await self._post(messages)

    async def _post(self, messages: list[dict[str, str]]) -> dict[str, Any]:
        payload: dict[str, Any] = {"messages": messages}
        if self._config.model:
            payload["model"] = self._config.model
        headers = {}
        if self._config.api_key:
            headers["Authorization"] = f"Bearer {self._config.api_key}"
        try:
            async with httpx.AsyncClient(
                timeout=self._config.timeout_seconds, transport=self._transport, headers=headers
            ) as client:
                response = await client.post(
                    f"{self._config.base_url}/chat/completions", json=payload
                )
                response.raise_for_status()
                return response.json()
        except (httpx.HTTPError, ValueError) as exc:
            raise AgentError(f"agent request failed: {exc}") from exc


class FakeAgentClient:
    """Scriptable agent for tests elsewhere in the suite."""

    def __init__(self, reply_text: str = "Done.", error: Exception | None = None) -> None:
        self._reply = reply_text
        self._error = error
        self.conversations: list[tuple[str, list[tuple[str, str]]]] = []
        self.ingested: list[tuple[str, int | None]] = []

    async def converse(self, text: str, history: list[tuple[str, str]]) -> str:
        if self._error:
            raise self._error
        self.conversations.append((text, history))
        return self._reply

    async def ingest(self, text: str, recorded_at: int | None) -> None:
        if self._error:
            raise self._error
        self.ingested.append((text, recorded_at))
