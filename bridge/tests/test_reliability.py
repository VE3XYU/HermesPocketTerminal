"""Cross-cutting tests for the guarantees in design section 8."""

import itertools

import pytest

from htp_bridge.agent import AgentError, FakeAgentClient
from htp_bridge.captures import CaptureStore
from htp_bridge.config import StorageConfig
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import FakeSpeechProvider, SpeechError
from htp_bridge.storage import AudioStorage
from tests.conftest import AUTH

WAV = b"RIFF" + b"\x00" * 64


def build_pipeline(db, fake_clock, tmp_path, *, speech, agent):
    captures = CaptureStore(db, clock=fake_clock)
    storage = AudioStorage(
        StorageConfig(
            db_path=tmp_path / "htp.db",
            audio_dir=tmp_path / "audio",
            upload_retention_days=365,
            reply_retention_days=7,
        ),
        clock=fake_clock,
    )
    counter = itertools.count(1)
    notifications = NotificationStore(db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}")
    pipeline = Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=speech,
        agent=agent,
        salutation_prefixes=["hey hermes"],
        clock=fake_clock,
    )
    return captures, storage, notifications, pipeline


def test_repeated_upload_delivers_the_note_exactly_once(client, app_context):
    headers = {**AUTH, "X-Capture-Id": "c-1", "X-Recorded-At": "5", "Content-Type": "audio/wav"}

    for _ in range(5):
        assert client.post("/htp/v1/captures", content=WAV, headers=headers).status_code == 200

    assert len(app_context["agent"].ingested) == 1, "idempotency must survive aggressive retries"
    assert app_context["captures"].get("c-1").state == "done"


def test_repeat_upload_does_not_reset_an_in_flight_capture(client, app_context):
    headers = {**AUTH, "X-Capture-Id": "c-1", "Content-Type": "audio/wav"}
    client.post("/htp/v1/captures", content=WAV, headers=headers)
    app_context["captures"].set_state("c-1", "failed", error="transcription_failed")

    response = client.post("/htp/v1/captures", content=WAV, headers=headers)

    assert response.json()["state"] == "failed"


@pytest.mark.parametrize("crash_state", ["received", "transcribing", "processing"])
async def test_capture_resumes_from_any_mid_pipeline_state(db, fake_clock, tmp_path, crash_state):
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = FakeAgentClient()
    captures, storage, _, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=agent
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)
    captures.set_state("c-1", crash_state)

    # A fresh Pipeline stands in for a restarted process: only durable state survives.
    _, _, _, restarted = build_pipeline(db, fake_clock, tmp_path, speech=speech, agent=agent)
    resumed = await restarted.resume()

    assert resumed == 1
    assert captures.get("c-1").state == "done"


async def test_no_capture_is_lost_when_transcription_is_down(db, fake_clock, tmp_path):
    speech = FakeSpeechProvider(transcribe_error=SpeechError("stt down"))
    captures, storage, _, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=FakeAgentClient()
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)

    await pipeline.process("c-1")

    assert captures.get("c-1").state == "failed"
    assert storage.upload_path("c-1").exists(), "the recording must survive a provider outage"


async def test_note_survives_agent_outage_and_is_delivered_on_recovery(db, fake_clock, tmp_path):
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    captures, storage, _, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)
    await pipeline.process("c-1")

    healthy = FakeAgentClient()
    _, _, _, recovered = build_pipeline(db, fake_clock, tmp_path, speech=speech, agent=healthy)
    await recovered.sweep_ingestion()

    assert healthy.ingested == [("Add milk", 1)]
    assert captures.get("c-1").error is None


async def test_unclaimed_reply_is_never_silently_dropped(db, fake_clock, tmp_path):
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, what's for dinner?"})
    captures, storage, notifications, pipeline = build_pipeline(
        db, fake_clock, tmp_path, speech=speech, agent=FakeAgentClient(reply_text="Lasagne at 7 PM.")
    )
    captures.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    storage.save_upload("c-1", WAV)
    await pipeline.process("c-1")

    fake_clock.advance(3600)  # device slept long ago
    await pipeline.sweep_redirects()

    assert [n.text for n in notifications.pending()] == ["Lasagne at 7 PM."]


def test_notifications_redeliver_across_polls_until_acknowledged(client, app_context):
    app_context["notifications"].enqueue("Meeting with Alex at 10:00 AM", "urgent")

    for _ in range(3):
        body = client.get("/htp/v1/notifications", headers=AUTH).json()
        assert len(body["notifications"]) == 1

    client.post("/htp/v1/notifications/ack", json={"ids": ["n-1"]}, headers=AUTH)
    assert client.get("/htp/v1/notifications", headers=AUTH).json()["notifications"] == []


def test_device_learns_of_captures_the_bridge_never_received(client):
    body = client.get("/htp/v1/captures", params={"ids": "c-lost"}, headers=AUTH).json()
    assert body["captures"][0]["state"] == "unknown"


def test_every_device_response_carries_server_time(client, app_context):
    app_context["dashboard"].publish("Today", [{"id": "t-1", "text": "Buy milk", "done": False}])
    for path, params in (
        ("/htp/v1/captures", {"ids": ""}),
        ("/htp/v1/dashboard", {}),
        ("/htp/v1/notifications", {}),
    ):
        body = client.get(path, params=params, headers=AUTH).json()
        assert body["server_time"] == app_context["clock"].now, f"{path} must carry server_time"


def test_oversized_upload_is_rejected_without_storing(client, app_context):
    headers = {**AUTH, "X-Capture-Id": "c-big", "Content-Type": "audio/wav"}
    client.post("/htp/v1/captures", content=b"x" * 5000, headers=headers)

    assert app_context["captures"].get("c-big") is None
    assert not app_context["storage"].upload_path("c-big").exists()
