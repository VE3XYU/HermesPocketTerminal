from __future__ import annotations

from pathlib import Path
from typing import Protocol

import httpx

from htp_bridge.config import SpeechConfig


class SpeechError(Exception):
    """Raised when a speech provider fails or is misconfigured."""


class SpeechProvider(Protocol):
    async def transcribe(self, wav_path: Path) -> str: ...

    async def synthesize(self, text: str) -> bytes: ...


class OpenAISpeechProvider:
    """Speech-to-text and text-to-speech over the OpenAI audio endpoints.

    `transport` is injectable so tests exercise the real request construction
    without touching the network.
    """

    def __init__(self, config: SpeechConfig, transport: httpx.BaseTransport | None = None) -> None:
        self._config = config
        self._transport = transport

    def _client(self) -> httpx.AsyncClient:
        return httpx.AsyncClient(
            timeout=self._config.timeout_seconds,
            transport=self._transport,
            headers={"Authorization": f"Bearer {self._config.api_key}"},
        )

    async def transcribe(self, wav_path: Path) -> str:
        try:
            async with self._client() as client:
                response = await client.post(
                    f"{self._config.base_url}/audio/transcriptions",
                    files={"file": (wav_path.name, wav_path.read_bytes(), "audio/wav")},
                    data={"model": self._config.stt_model},
                )
                response.raise_for_status()
                data = response.json()
                if not isinstance(data, dict):
                    raise SpeechError(f"transcription failed: expected dict, got {type(data).__name__}")
                return str(data.get("text", "")).strip()
        except (httpx.HTTPError, ValueError) as exc:
            raise SpeechError(f"transcription failed: {exc}") from exc

    async def synthesize(self, text: str) -> bytes:
        try:
            async with self._client() as client:
                response = await client.post(
                    f"{self._config.base_url}/audio/speech",
                    json={
                        "model": self._config.tts_model,
                        "voice": self._config.tts_voice,
                        "input": text,
                        "response_format": "wav",
                    },
                )
                response.raise_for_status()
                return response.content
        except httpx.HTTPError as exc:
            raise SpeechError(f"synthesis failed: {exc}") from exc


class FakeSpeechProvider:
    """Deterministic provider for tests elsewhere in the suite."""

    def __init__(
        self,
        transcripts: dict[str, str] | None = None,
        default_transcript: str = "",
        transcribe_error: Exception | None = None,
        synthesize_error: Exception | None = None,
    ) -> None:
        self._transcripts = transcripts or {}
        self._default = default_transcript
        self._transcribe_error = transcribe_error
        self._synthesize_error = synthesize_error
        self.synthesized: list[str] = []

    def set_default_transcript(self, text: str) -> None:
        """Set the transcript returned when no per-file script matches."""
        self._default = text

    async def transcribe(self, wav_path: Path) -> str:
        if self._transcribe_error:
            raise self._transcribe_error
        return self._transcripts.get(wav_path.stem, self._default)

    async def synthesize(self, text: str) -> bytes:
        if self._synthesize_error:
            raise self._synthesize_error
        self.synthesized.append(text)
        return b"RIFF-fake-reply"


def build_speech_provider(config: SpeechConfig) -> SpeechProvider:
    if config.provider == "openai":
        return OpenAISpeechProvider(config)
    raise SpeechError(f"unknown speech provider '{config.provider}'")
