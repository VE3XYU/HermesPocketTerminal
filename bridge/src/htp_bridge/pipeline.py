from __future__ import annotations

import logging
import time
import uuid
from typing import Callable

from htp_bridge import salutation
from htp_bridge.agent import AgentError
from htp_bridge.captures import INGEST_FAILED, TERMINAL_STATES, CaptureStore
from htp_bridge.notifications import NotificationStore
from htp_bridge.speech import SpeechError
from htp_bridge.storage import AudioStorage

log = logging.getLogger(__name__)


def _default_conversation_id() -> str:
    return f"v-{uuid.uuid4().hex[:8]}"


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
    ) -> None:
        self._captures = captures
        self._storage = storage
        self._notifications = notifications
        self._speech = speech
        self._agent = agent
        self._prefixes = salutation_prefixes
        self._grace = reply_grace_seconds
        self._clock = clock
        self._new_conversation_id = conversation_id_factory

    async def process(self, capture_id: str) -> None:
        capture = self._captures.get(capture_id)
        if capture is None or capture.state in TERMINAL_STATES:
            return

        self._captures.set_state(capture_id, "transcribing")
        try:
            transcript = await self._speech.transcribe(self._storage.upload_path(capture_id))
        except (SpeechError, OSError):
            log.exception("capture %s: transcription failed", capture_id)
            if self._storage.upload_path(capture_id).exists():
                error = "transcription_failed"
            else:
                error = "audio_missing"
            self._captures.set_state(capture_id, "failed", error=error)
            return

        self._captures.set_transcript(capture_id, transcript)
        remainder = salutation.detect(transcript, self._prefixes)

        if remainder is None:
            await self._finish_note(capture_id, transcript, capture.recorded_at)
        else:
            await self._answer(capture_id, remainder or transcript, capture.conversation_id)

    async def _finish_note(self, capture_id: str, transcript: str, recorded_at: int | None) -> None:
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
        try:
            await self._agent.ingest(transcript, recorded_at)
        except AgentError:
            log.exception("capture %s: agent ingestion failed, queued for retry", capture_id)
            return
        self._captures.set_state(capture_id, "done", error=None)

    async def _answer(self, capture_id: str, prompt: str, conversation_id: str | None) -> None:
        self._captures.set_state(capture_id, "processing")
        conversation_id = conversation_id or self._new_conversation_id()
        self._captures.set_conversation(capture_id, conversation_id)
        history = self._captures.conversation_history(conversation_id, exclude_id=capture_id)

        try:
            reply = await self._agent.converse(prompt, history)
        except AgentError:
            log.exception("capture %s: agent unavailable", capture_id)
            self._captures.set_state(capture_id, "failed", error="agent_unavailable")
            return

        try:
            audio = await self._speech.synthesize(reply)
        except SpeechError:
            log.exception("capture %s: synthesis failed", capture_id)
            self._captures.set_state(capture_id, "failed", error="synthesis_failed")
            return

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
            if not capture.transcript:
                continue
            try:
                await self._agent.ingest(capture.transcript, capture.recorded_at)
            except AgentError:
                log.warning("capture %s: agent still unavailable for ingestion", capture.id)
                continue
            self._captures.set_state(capture.id, "done", error=None)
            retried += 1
        return retried
