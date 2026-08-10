from __future__ import annotations

import logging
import time
import uuid
from typing import Callable

from htp_bridge import salutation
from htp_bridge.agent import AgentError
from htp_bridge.captures import INGEST_FAILED, TERMINAL_STATES, Capture, CaptureStore
from htp_bridge.notifications import NotificationStore
from htp_bridge.speech import SpeechError
from htp_bridge.storage import AudioStorage
from htp_bridge.timing import CaptureTimer, NullTimingStore, TimingStore, log_timing

log = logging.getLogger(__name__)


def _default_conversation_id() -> str:
    return f"v-{uuid.uuid4().hex[:8]}"


def _file_size(path) -> int | None:
    """Upload size for the timing row. Absent audio is a real case (the crash
    window resume() documents), so a missing file records nothing rather than
    raising inside instrumentation."""
    try:
        return path.stat().st_size
    except OSError:
        return None


class Pipeline:
    """Moves a capture from stored audio to a terminal state.

    Every stage records its outcome in the capture row before the next begins, so
    a process that dies mid-pipeline can be resumed from durable state rather than
    memory.
    """

    def __init__(
        self,
        *,
        captures: CaptureStore,
        storage: AudioStorage,
        notifications: NotificationStore,
        speech,
        agent,
        salutation_prefixes: list[str],
        reply_grace_seconds: int = 90,
        clock: Callable[[], int] = lambda: int(time.time()),
        conversation_id_factory: Callable[[], str] = _default_conversation_id,
        timings: TimingStore | None = None,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        # Captures whose ingest call is in flight right now. Deliberately in
        # memory and never persisted: the ingest-failed flag on disk cannot tell
        # a slow agent from a failed one, and after a crash the set is empty --
        # which is correct, because an interrupted ingest does need retrying.
        self._ingesting: set[str] = set()
        self._captures = captures
        self._storage = storage
        self._notifications = notifications
        self._speech = speech
        self._agent = agent
        self._prefixes = salutation_prefixes
        self._grace = reply_grace_seconds
        self._clock = clock
        self._new_conversation_id = conversation_id_factory
        self._timings = timings or NullTimingStore()
        self._monotonic = monotonic

    async def process(self, capture_id: str) -> None:
        capture = self._captures.get(capture_id)
        if capture is None or capture.state in TERMINAL_STATES:
            return

        # Created after the early return: a duplicate upload of a finished
        # capture does no work and should not look like a measured run.
        timer = CaptureTimer(capture_id, monotonic=self._monotonic, clock=self._clock)
        try:
            await self._process(capture, timer)
        finally:
            self._record_timing(capture_id, timer)

    async def _process(self, capture: Capture, timer: CaptureTimer) -> None:
        capture_id = capture.id
        self._captures.set_state(capture_id, "transcribing")
        upload_path = self._storage.upload_path(capture_id)
        timer.note(audio_bytes=_file_size(upload_path))
        try:
            with timer.stage("transcribe"):
                transcript = await self._speech.transcribe(upload_path)
        except (SpeechError, OSError):
            log.exception("capture %s: transcription failed", capture_id)
            if upload_path.exists():
                error = "transcription_failed"
            else:
                error = "audio_missing"
            self._captures.set_state(capture_id, "failed", error=error)
            return

        timer.note(transcript_chars=len(transcript))
        self._captures.set_transcript(capture_id, transcript)
        remainder = salutation.detect(transcript, self._prefixes)

        # Salutation detection (design §6.2) decides disposition only for captures
        # that arrive without a conversation ID. A capture whose conversation_id is
        # already set was uploaded with the device echoing the ID this bridge handed
        # back on the previous turn (§5.1), which means the user is answering a
        # spoken reply -- and follow-ups are spoken naturally, without repeating the
        # salutation. Routing such a capture by salutation alone turned every
        # follow-up into an orphaned note. The prompt is still the stripped
        # remainder when a salutation is present, and the full transcript otherwise.
        if remainder is None and capture.conversation_id is None:
            timer.set_kind("note")
            await self._finish_note(capture_id, transcript, capture.recorded_at, timer)
        else:
            timer.set_kind("conversation")
            await self._answer(
                capture_id, remainder or transcript, capture.conversation_id, timer
            )

    def _record_timing(self, capture_id: str, timer: CaptureTimer) -> None:
        """Write the timing row. Swallows its own failures on purpose: a
        diagnostic that can lose someone's note is worse than no diagnostic.
        Runs in a `finally`, so an exception from the pipeline body still
        propagates."""
        try:
            capture = self._captures.get(capture_id)
            outcome = capture.error if capture is not None and capture.error else "ok"
            timing = timer.finish(outcome)
            log_timing(timing)
            self._timings.record(timing)
        except Exception:
            log.exception("capture %s: failed to record timing", capture_id)

    async def _finish_note(
        self, capture_id: str, transcript: str, recorded_at: int | None, timer: CaptureTimer
    ) -> None:
        """Mark the note done first: the device is waiting, the agent is not.

        The ingest-failed flag is written *before* calling ingest, not only after
        a caught AgentError. A process that dies between marking the row done and
        hearing back from the agent would otherwise leave the row `done` with
        `error IS NULL`: not active (resume() would skip it) and not in the
        ingestion backlog (which requires error=INGEST_FAILED) -- the note would
        be silently lost. Writing the flag first means an interrupted ingest is
        always visible to sweep_ingestion(); a clean success clears it again.
        """
        self._captures.set_state(capture_id, "done", error=INGEST_FAILED)
        self._ingesting.add(capture_id)
        try:
            with timer.stage("agent"):
                await self._agent.ingest(transcript, recorded_at)
        except AgentError:
            log.exception("capture %s: agent ingestion failed, queued for retry", capture_id)
            return
        finally:
            self._ingesting.discard(capture_id)
        self._captures.set_state(capture_id, "done", error=None)

    async def _answer(
        self, capture_id: str, prompt: str, conversation_id: str | None, timer: CaptureTimer
    ) -> None:
        self._captures.set_state(capture_id, "processing")
        conversation_id = conversation_id or self._new_conversation_id()
        self._captures.set_conversation(capture_id, conversation_id)
        history = self._captures.conversation_history(conversation_id, exclude_id=capture_id)

        try:
            with timer.stage("agent"):
                reply = await self._agent.converse(prompt, history)
        except AgentError:
            log.exception("capture %s: agent unavailable", capture_id)
            self._captures.set_state(capture_id, "failed", error="agent_unavailable")
            return

        timer.note(reply_chars=len(reply))
        try:
            with timer.stage("synthesize"):
                audio = await self._speech.synthesize(reply)
        except SpeechError:
            log.exception("capture %s: synthesis failed", capture_id)
            self._captures.set_state(capture_id, "failed", error="synthesis_failed")
            return

        timer.note(reply_bytes=len(audio))
        with timer.stage("save"):
            self._storage.save_reply(capture_id, audio)
            self._captures.set_reply(capture_id, reply)

    async def resume(self) -> int:
        """Reprocess captures that were mid-pipeline when the process last stopped.

        Must complete before the API begins accepting uploads: concurrent
        invocation with live process() calls can double-deliver (e.g. ingest a
        note twice, or open a second conversation turn for a capture still
        being resumed). The server wires this in at startup, ahead of
        accepting requests -- resume() itself takes no lock.

        Deviation from the brief (human-approved): a crash window can leave a
        capture row without its upload WAV ever landing on disk. A real
        SpeechProvider reads that file directly, so a missing file surfaces as
        FileNotFoundError -- not a SpeechError -- out of process(). Left
        unguarded, that exception would escape this loop and abort resume()
        entirely, wedging every other unfinished capture behind the one with
        the missing file. Each capture's process() call is therefore isolated:
        an unexpected exception marks just that capture failed and the loop
        continues. (process() now also catches OSError directly at the
        transcription stage, so this wrapper is a safety net for other
        unforeseen failures rather than the primary defense against a missing
        WAV -- that defense now lives on the direct-call path too.)

        Fix (review): the failure branch must not blindly overwrite the
        capture's state. _finish_note writes done/ingest_failed *before*
        calling agent.ingest, precisely so an interrupted ingest stays visible
        to sweep_ingestion(). If a non-AgentError then escapes process() (e.g.
        a bare exception from a still-misbehaving agent client), blindly
        setting the row to failed/pipeline_error here would erase that
        write-ahead flag -- removing the capture from ingestion_backlog() and
        losing the note permanently instead of leaving it for retry. The row
        is therefore re-fetched and only overwritten if it hasn't already
        reached a terminal state.
        """
        pending = self._captures.unfinished_ids()
        for capture_id in pending:
            try:
                await self.process(capture_id)
            except Exception:
                log.exception("capture %s: unexpected error during resume", capture_id)
                capture = self._captures.get(capture_id)
                if capture is None or capture.state in TERMINAL_STATES:
                    continue
                if not self._storage.upload_path(capture_id).exists():
                    error = "audio_missing"
                else:
                    error = "pipeline_error"
                self._captures.set_state(capture_id, "failed", error=error)
        return len(pending)

    async def sweep_redirects(self) -> int:
        """Move unclaimed replies into the notification queue.

        The device sleeps after its polling window, so a reply that was never
        downloaded would otherwise be lost. Sending it as a notification means the
        answer arrives on the next sync instead.
        """
        cutoff = self._clock() - self._grace
        redirected = 0
        for capture in self._captures.redirect_candidates(older_than=cutoff):
            if not capture.reply_text:
                continue
            self._notifications.enqueue(capture.reply_text, "normal")
            self._captures.mark_redirected(capture.id)
            redirected += 1
        return redirected

    async def sweep_ingestion(self) -> int:
        """Retry notes the agent refused or was unavailable for."""
        retried = 0
        for capture in self._captures.ingestion_backlog():
            if not capture.transcript or capture.id in self._ingesting:
                continue
            try:
                await self._agent.ingest(capture.transcript, capture.recorded_at)
            except AgentError:
                log.warning("capture %s: agent still unavailable for ingestion", capture.id)
                continue
            self._captures.set_state(capture.id, "done", error=None)
            retried += 1
        return retried
