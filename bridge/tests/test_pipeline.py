import itertools

import pytest

from htp_bridge.agent import AgentError, FakeAgentClient
from htp_bridge.captures import CaptureStore
from htp_bridge.config import StorageConfig
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import FakeSpeechProvider, SpeechError
from htp_bridge.storage import AudioStorage

PREFIXES = ["hey hermes", "hermes"]


@pytest.fixture
def parts(db, fake_clock, tmp_path):
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
    notifications = NotificationStore(
        db, clock=fake_clock, id_factory=lambda: f"n-{next(counter)}"
    )
    return captures, storage, notifications


def build(parts, fake_clock, *, speech, agent, grace=90):
    captures, storage, notifications = parts
    conversations = itertools.count(1)
    return Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=speech,
        agent=agent,
        salutation_prefixes=PREFIXES,
        reply_grace_seconds=grace,
        clock=fake_clock,
        conversation_id_factory=lambda: f"v-{next(conversations)}",
    )


def upload(parts, capture_id, *, conversation_id=None):
    captures, storage, _ = parts
    captures.create(
        capture_id=capture_id,
        device_id="pocket-01",
        recorded_at=1,
        conversation_id=conversation_id,
    )
    storage.save_upload(capture_id, b"RIFFfake")


async def test_note_reaches_done_without_entering_processing(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk to the shopping list"})
    agent = FakeAgentClient()
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "done"
    assert capture.transcript == "Add milk to the shopping list"
    assert capture.reply_text is None
    assert agent.ingested == [("Add milk to the shopping list", 1)]
    assert agent.conversations == [], "a note must never open a conversation"
    assert speech.synthesized == [], "a note must never be synthesized"


async def test_salutation_routes_to_conversation_and_strips_the_prefix(parts, fake_clock):
    captures, storage, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, what's on my calendar?"})
    agent = FakeAgentClient(reply_text="You have one meeting at 10:00 AM.")
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "reply_ready"
    assert capture.transcript == "Hey Hermes, what's on my calendar?"
    assert capture.reply_text == "You have one meeting at 10:00 AM."
    assert capture.conversation_id == "v-1"
    assert agent.conversations[0][0] == "what's on my calendar?"
    assert storage.has_reply("c-1")
    assert agent.ingested == []


async def test_salutation_only_falls_back_to_the_full_transcript(parts, fake_clock):
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes"})
    agent = FakeAgentClient()
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    assert agent.conversations[0][0] == "Hey Hermes"


async def test_follow_up_reuses_conversation_and_sends_history(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(
        transcripts={"c-1": "Hey Hermes, what time is dinner?", "c-2": "Hey Hermes, and dessert?"}
    )
    agent = FakeAgentClient(reply_text="Dinner is at 7 PM.")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent)

    upload(parts, "c-1")
    await pipeline.process("c-1")
    upload(parts, "c-2", conversation_id="v-1")
    await pipeline.process("c-2")

    assert captures.get("c-2").conversation_id == "v-1"
    assert agent.conversations[1][1] == [("Hey Hermes, what time is dinner?", "Dinner is at 7 PM.")]


async def test_transcription_failure_marks_capture_failed(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcribe_error=SpeechError("stt down"))
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=FakeAgentClient()).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "failed"
    assert capture.error == "transcription_failed"


async def test_process_marks_failed_audio_missing_when_wav_absent(parts, fake_clock):
    """Fix for review Finding 2: process() must catch OSError (not just
    SpeechError) at the transcription stage. A real SpeechProvider reads the
    WAV file directly, so a missing file surfaces as FileNotFoundError, and
    the API calls process() directly (not only via resume()) -- so this must
    hold on the primary path, not just after a crash-and-resume.
    """
    captures, storage, _ = parts
    speech = FakeSpeechProvider(transcribe_error=FileNotFoundError("no such file"))
    agent = FakeAgentClient()
    captures.create(
        capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None
    )
    # Deliberately do not save the upload: the WAV never landed on disk.
    assert not storage.upload_path("c-1").exists()

    await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "failed"
    assert capture.error == "audio_missing"


async def test_agent_failure_on_conversation_marks_capture_failed_but_keeps_transcript(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, hello"})
    upload(parts, "c-1")

    await build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    ).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "failed"
    assert capture.error == "agent_unavailable"
    assert capture.transcript == "Hey Hermes, hello", "the recording is still transcribed"


async def test_synthesis_failure_marks_capture_failed(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(
        transcripts={"c-1": "Hey Hermes, hello"}, synthesize_error=SpeechError("tts down")
    )
    upload(parts, "c-1")

    await build(parts, fake_clock, speech=speech, agent=FakeAgentClient()).process("c-1")

    assert captures.get("c-1").error == "synthesis_failed"


async def test_ingestion_failure_leaves_note_done_and_flags_backlog(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    upload(parts, "c-1")

    await build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    ).process("c-1")

    capture = captures.get("c-1")
    assert capture.state == "done", "the device already confirmed and slept"
    assert capture.error == "ingest_failed"
    assert [c.id for c in captures.ingestion_backlog()] == ["c-1"]


async def test_ingest_crash_before_completion_still_flags_backlog(parts, fake_clock):
    """Fix for review Finding 1: the done/ingest_failed write must happen
    BEFORE the ingest call, not only after a caught AgentError. Otherwise a
    process that dies mid-ingest leaves the row done/error=None -- invisible
    to both resume() (terminal, skipped) and ingestion_backlog() (requires
    error=ingest_failed) -- and the note is silently lost.

    FakeAgentClient can only raise the single scripted error, and process()
    only catches AgentError, so a bare exception from ingest() is used here
    to simulate the crash window and prove the pre-write already landed.
    """
    captures, _, _ = parts

    class ExplodingAgent:
        def __init__(self) -> None:
            self.ingest_called = False

        async def ingest(self, text, recorded_at):
            self.ingest_called = True
            raise RuntimeError("simulated crash mid-ingest")

        async def converse(self, text, history):
            raise AssertionError("not used in this test")

    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = ExplodingAgent()
    upload(parts, "c-1")

    with pytest.raises(RuntimeError):
        await build(parts, fake_clock, speech=speech, agent=agent).process("c-1")

    assert agent.ingest_called
    capture = captures.get("c-1")
    assert capture.state == "done", "the device already confirmed and slept"
    assert capture.error == "ingest_failed"
    assert [c.id for c in captures.ingestion_backlog()] == ["c-1"]


async def test_sweep_ingestion_retries_and_clears_the_flag(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    upload(parts, "c-1")
    await build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(error=AgentError("down"))
    ).process("c-1")

    healthy = FakeAgentClient()
    retried = await build(parts, fake_clock, speech=speech, agent=healthy).sweep_ingestion()

    assert retried == 1
    assert captures.get("c-1").error is None
    assert healthy.ingested == [("Add milk", 1)]


async def test_process_is_a_noop_for_terminal_captures(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = FakeAgentClient()
    upload(parts, "c-1")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent)

    await pipeline.process("c-1")
    await pipeline.process("c-1")

    assert len(agent.ingested) == 1, "reprocessing must not deliver the note twice"


async def test_process_ignores_unknown_capture(parts, fake_clock):
    captures, _, _ = parts
    agent = FakeAgentClient()
    pipeline = build(parts, fake_clock, speech=FakeSpeechProvider(), agent=agent)

    await pipeline.process("c-nope")

    assert captures.get("c-nope") is None
    assert agent.ingested == []
    assert agent.conversations == []


async def test_resume_reprocesses_captures_left_mid_pipeline(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = FakeAgentClient()
    upload(parts, "c-1")
    captures.set_state("c-1", "transcribing")  # simulate a crash mid-stage

    resumed = await build(parts, fake_clock, speech=speech, agent=agent).resume()

    assert resumed == 1
    assert captures.get("c-1").state == "done"


async def test_resume_does_not_clobber_ingest_failed_when_agent_raises_unexpectedly(parts, fake_clock):
    """Fix round 2: resume()'s per-capture except must not blindly overwrite the
    row to failed/pipeline_error. _finish_note writes done/ingest_failed BEFORE
    calling agent.ingest, precisely so an interrupted ingest is visible to
    sweep_ingestion(). If agent.ingest raises something other than AgentError
    (simulating a still-misbehaving agent client) after that write-ahead flag
    has landed, resume()'s safety net must leave the row alone -- clobbering it
    would silently drop the note instead of leaving it for sweep_ingestion() to
    retry.
    """
    captures, _, _ = parts

    class ExplodingAgent:
        async def ingest(self, text, recorded_at):
            raise RuntimeError("simulated ongoing agent misbehavior")

        async def converse(self, text, history):
            raise AssertionError("not used in this test")

    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk"})
    agent = ExplodingAgent()
    upload(parts, "c-1")
    captures.set_state("c-1", "received")  # simulate a crash before this capture finished

    resumed = await build(parts, fake_clock, speech=speech, agent=agent).resume()

    assert resumed == 1
    capture = captures.get("c-1")
    assert capture.state == "done", "the write-ahead flag must survive, not be clobbered to failed"
    assert capture.error == "ingest_failed"
    assert [c.id for c in captures.ingestion_backlog()] == ["c-1"]


async def test_sweep_redirects_queues_unclaimed_reply_as_notification(parts, fake_clock):
    captures, _, notifications = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, what's for dinner?"})
    agent = FakeAgentClient(reply_text="Lasagne at 7 PM.")
    upload(parts, "c-1")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent, grace=90)
    await pipeline.process("c-1")

    fake_clock.advance(91)
    redirected = await pipeline.sweep_redirects()

    assert redirected == 1
    assert [n.text for n in notifications.pending()] == ["Lasagne at 7 PM."]
    assert captures.get("c-1").redirected is True


async def test_sweep_redirects_leaves_fresh_and_downloaded_replies_alone(parts, fake_clock):
    captures, _, notifications = parts
    speech = FakeSpeechProvider(
        transcripts={"c-1": "Hey Hermes, hello", "c-2": "Hey Hermes, hello"}
    )
    agent = FakeAgentClient(reply_text="Hi.")
    pipeline = build(parts, fake_clock, speech=speech, agent=agent, grace=90)
    upload(parts, "c-1")
    await pipeline.process("c-1")
    captures.mark_downloaded("c-1")
    fake_clock.advance(91)
    upload(parts, "c-2")
    await pipeline.process("c-2")

    assert await pipeline.sweep_redirects() == 0
    assert notifications.pending() == []


async def test_sweep_redirects_runs_once_per_capture(parts, fake_clock):
    _, _, notifications = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Hey Hermes, hello"})
    pipeline = build(
        parts, fake_clock, speech=speech, agent=FakeAgentClient(reply_text="Hi."), grace=90
    )
    upload(parts, "c-1")
    await pipeline.process("c-1")
    fake_clock.advance(91)

    await pipeline.sweep_redirects()
    await pipeline.sweep_redirects()

    assert len(notifications.pending()) == 1


# --- Approved deviation: resume() must be robust to a missing upload WAV. ---
#
# The brief's pipeline assumes the WAV exists when reprocessing an unfinished
# capture after a crash. A crash window can leave a capture row without its
# audio file (e.g. the row was written but the file write never landed or was
# lost). A real SpeechProvider's transcribe() reads the file directly, so a
# missing file surfaces as FileNotFoundError -- not a SpeechError -- and left
# unguarded that would escape resume()'s loop and wedge every other unfinished
# capture behind it. FakeSpeechProvider never touches the filesystem, so these
# tests script it with `transcribe_error=FileNotFoundError(...)` to stand in
# for what a real provider raises when the file is gone.
#
# Note: process() itself now also catches OSError at the transcription stage
# (review Finding 2, see test_process_marks_failed_audio_missing_when_wav_absent
# above), so these two tests now exercise that same handling reached through
# resume() rather than resume()'s own try/except wrapper -- the end-to-end
# crash-resume behavior they assert (resume() doesn't wedge, both captures
# reach a terminal state) still holds and is still worth covering here.


async def test_resume_marks_capture_failed_when_upload_wav_is_missing(parts, fake_clock):
    captures, storage, _ = parts
    speech = FakeSpeechProvider(transcribe_error=FileNotFoundError("no such file"))
    agent = FakeAgentClient()
    captures.create(
        capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None
    )
    # Deliberately do not save the upload: the WAV never landed on disk.
    captures.set_state("c-1", "received")
    assert not storage.upload_path("c-1").exists()

    resumed = await build(parts, fake_clock, speech=speech, agent=agent).resume()

    assert resumed == 1
    capture = captures.get("c-1")
    assert capture.state == "failed"
    assert capture.error == "audio_missing"


async def test_resume_continues_past_a_capture_with_missing_audio(parts, fake_clock):
    captures, storage, _ = parts
    agent = FakeAgentClient()

    # c-1: received, but its upload WAV is missing -- must not abort resume().
    captures.create(
        capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None
    )
    assert not storage.upload_path("c-1").exists()

    # Advance the clock so c-1 and c-2 have distinct created_at values: resume()
    # orders unfinished_ids() by created_at, and without this the two captures
    # would tie, letting the test pass even if the missing-WAV capture happened
    # to be processed last (i.e. even if it never actually got in front of c-2).
    fake_clock.advance(1)

    # c-2: received, with an intact WAV -- must still be processed to a terminal state.
    upload(parts, "c-2")

    # FakeSpeechProvider only supports one blanket transcribe_error, and it never
    # touches the filesystem, so it can't naturally fail for one capture but not
    # another. This subclass fails exactly like a real provider would: only when
    # the file it was asked to read is actually absent.
    class MissingAwareSpeech(FakeSpeechProvider):
        async def transcribe(self, wav_path):
            if not wav_path.exists():
                raise FileNotFoundError(f"no such file: {wav_path}")
            return await super().transcribe(wav_path)

    speech = MissingAwareSpeech(transcripts={"c-2": "Add milk"})

    resumed = await build(parts, fake_clock, speech=speech, agent=agent).resume()

    assert resumed == 2
    assert captures.get("c-1").state == "failed"
    assert captures.get("c-1").error == "audio_missing"
    assert captures.get("c-2").state == "done"
    assert agent.ingested == [("Add milk", 1)]
