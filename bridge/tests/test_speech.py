import json

import httpx
import pytest

from htp_bridge.config import SpeechConfig
from htp_bridge.speech import (
    FakeSpeechProvider,
    OpenAISpeechProvider,
    SpeechError,
    build_speech_provider,
)


@pytest.fixture
def speech_config():
    return SpeechConfig(
        provider="openai",
        api_key="sk-test",
        stt_model="whisper-1",
        tts_model="tts-1",
        tts_voice="alloy",
        timeout_seconds=30,
        base_url="https://api.example.com/v1",
    )


def provider_with(speech_config, handler):
    transport = httpx.MockTransport(handler)
    return OpenAISpeechProvider(speech_config, transport=transport)


async def test_transcribe_posts_audio_and_returns_text(speech_config, tmp_path):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["url"] = str(request.url)
        seen["auth"] = request.headers.get("authorization")
        seen["body"] = request.content
        return httpx.Response(200, json={"text": "Add milk to the shopping list"})

    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")

    text = await provider_with(speech_config, handler).transcribe(wav)

    assert text == "Add milk to the shopping list"
    assert seen["url"] == "https://api.example.com/v1/audio/transcriptions"
    assert seen["auth"] == "Bearer sk-test"
    assert b"whisper-1" in seen["body"]


async def test_transcribe_strips_surrounding_whitespace(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")
    handler = lambda request: httpx.Response(200, json={"text": "  hello  "})
    assert await provider_with(speech_config, handler).transcribe(wav) == "hello"


async def test_transcribe_raises_speech_error_on_http_failure(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")
    handler = lambda request: httpx.Response(500, text="upstream exploded")

    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).transcribe(wav)


async def test_transcribe_raises_speech_error_on_timeout(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")

    def handler(request):
        raise httpx.ConnectTimeout("too slow")

    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).transcribe(wav)


async def test_transcribe_raises_speech_error_on_non_dict_json(speech_config, tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"RIFFfake")
    handler = lambda request: httpx.Response(200, json=[1, 2, 3])

    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).transcribe(wav)


async def test_synthesize_requests_wav_and_returns_bytes(speech_config):
    seen = {}

    def handler(request: httpx.Request) -> httpx.Response:
        seen["url"] = str(request.url)
        seen["payload"] = json.loads(request.content)
        return httpx.Response(200, content=b"RIFFreply")

    audio = await provider_with(speech_config, handler).synthesize("Added milk.")

    assert audio == b"RIFFreply"
    assert seen["url"] == "https://api.example.com/v1/audio/speech"
    assert seen["payload"]["response_format"] == "wav"
    assert seen["payload"]["voice"] == "alloy"
    assert seen["payload"]["input"] == "Added milk."


async def test_synthesize_raises_speech_error_on_http_failure(speech_config):
    handler = lambda request: httpx.Response(429, text="slow down")
    with pytest.raises(SpeechError):
        await provider_with(speech_config, handler).synthesize("hello")


def test_build_speech_provider_returns_openai_implementation(speech_config):
    assert isinstance(build_speech_provider(speech_config), OpenAISpeechProvider)


def test_build_speech_provider_rejects_unknown_provider(speech_config):
    config = SpeechConfig(**{**speech_config.__dict__, "provider": "acme"})
    with pytest.raises(SpeechError, match="acme"):
        build_speech_provider(config)


async def test_fake_provider_returns_scripted_values(tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"x")
    fake = FakeSpeechProvider(transcripts={"c-1": "hello there"})

    assert await fake.transcribe(wav) == "hello there"
    assert await fake.synthesize("reply") == b"RIFF-fake-reply"
    assert fake.synthesized == ["reply"]


async def test_fake_provider_can_raise(tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"x")
    fake = FakeSpeechProvider(transcribe_error=SpeechError("stt down"))
    with pytest.raises(SpeechError):
        await fake.transcribe(wav)


async def test_fake_provider_set_default_transcript(tmp_path):
    wav = tmp_path / "c-1.wav"
    wav.write_bytes(b"x")
    fake = FakeSpeechProvider()
    fake.set_default_transcript("new default")
    assert await fake.transcribe(wav) == "new default"
