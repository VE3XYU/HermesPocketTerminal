from __future__ import annotations

import sqlite3
import time
from dataclasses import dataclass
from typing import Callable

from htp_bridge.db import Database

ACTIVE_STATES = ("received", "transcribing", "processing")
TERMINAL_STATES = ("done", "reply_ready", "failed")
INGEST_FAILED = "ingest_failed"


@dataclass(frozen=True)
class Capture:
    id: str
    device_id: str
    state: str
    transcript: str | None
    error: str | None
    conversation_id: str | None
    reply_text: str | None
    recorded_at: int | None
    created_at: int
    updated_at: int
    reply_ready_at: int | None
    reply_downloaded_at: int | None
    redirected: bool


@dataclass(frozen=True)
class ReplyLatency:
    """How long a conversation took, split at the point the device took over.

    `pipeline_seconds` is bridge work; `download_seconds` is how long the device
    took to notice reply_ready and fetch the WAV -- time no bridge-side
    optimization can touch.
    """

    capture_id: str
    pipeline_seconds: int
    download_seconds: int | None


def _to_capture(row: sqlite3.Row) -> Capture:
    return Capture(
        id=row["id"],
        device_id=row["device_id"],
        state=row["state"],
        transcript=row["transcript"],
        error=row["error"],
        conversation_id=row["conversation_id"],
        reply_text=row["reply_text"],
        recorded_at=row["recorded_at"],
        created_at=row["created_at"],
        updated_at=row["updated_at"],
        reply_ready_at=row["reply_ready_at"],
        reply_downloaded_at=row["reply_downloaded_at"],
        redirected=bool(row["redirected"]),
    )


class CaptureStore:
    def __init__(self, db: Database, clock: Callable[[], int] = lambda: int(time.time())) -> None:
        self._db = db
        self._clock = clock

    def create(
        self,
        *,
        capture_id: str,
        device_id: str,
        recorded_at: int | None,
        conversation_id: str | None,
    ) -> tuple[Capture, bool]:
        """Create a capture, or return the existing one untouched.

        The INSERT OR IGNORE makes this atomic: a duplicate upload can never
        produce a second row or reset an in-flight capture's state.
        """
        now = self._clock()
        with self._db.write() as conn:
            cursor = conn.execute(
                """
                INSERT OR IGNORE INTO captures
                    (id, device_id, state, recorded_at, conversation_id, created_at, updated_at)
                VALUES (?,?,?,?,?,?,?)
                """,
                (capture_id, device_id, "received", recorded_at, conversation_id, now, now),
            )
            created = cursor.rowcount == 1
            row = conn.execute("SELECT * FROM captures WHERE id=?", (capture_id,)).fetchone()
        return _to_capture(row), created

    def get(self, capture_id: str) -> Capture | None:
        with self._db.read() as conn:
            row = conn.execute("SELECT * FROM captures WHERE id=?", (capture_id,)).fetchone()
        return _to_capture(row) if row else None

    def get_many(self, capture_ids: list[str]) -> list[Capture]:
        """Return captures in the order requested, silently skipping unknown IDs."""
        if not capture_ids:
            return []
        placeholders = ",".join("?" for _ in capture_ids)
        with self._db.read() as conn:
            rows = conn.execute(
                f"SELECT * FROM captures WHERE id IN ({placeholders})", tuple(capture_ids)
            ).fetchall()
        by_id = {row["id"]: _to_capture(row) for row in rows}
        return [by_id[cid] for cid in capture_ids if cid in by_id]

    def set_state(self, capture_id: str, state: str, *, error: str | None = None) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET state=?, error=?, updated_at=? WHERE id=?",
                (state, error, self._clock(), capture_id),
            )

    def set_transcript(self, capture_id: str, transcript: str) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET transcript=?, updated_at=? WHERE id=?",
                (transcript, self._clock(), capture_id),
            )

    def set_conversation(self, capture_id: str, conversation_id: str) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET conversation_id=?, updated_at=? WHERE id=?",
                (conversation_id, self._clock(), capture_id),
            )

    def set_reply(self, capture_id: str, reply_text: str) -> None:
        now = self._clock()
        with self._db.write() as conn:
            conn.execute(
                """
                UPDATE captures
                   SET state='reply_ready', reply_text=?, reply_ready_at=?, error=NULL, updated_at=?
                 WHERE id=?
                """,
                (reply_text, now, now, capture_id),
            )

    def mark_downloaded(self, capture_id: str) -> None:
        now = self._clock()
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET reply_downloaded_at=?, updated_at=? WHERE id=?",
                (now, now, capture_id),
            )

    def unfinished_ids(self) -> list[str]:
        """Captures that were mid-pipeline when the process last stopped."""
        placeholders = ",".join("?" for _ in ACTIVE_STATES)
        with self._db.read() as conn:
            rows = conn.execute(
                f"SELECT id FROM captures WHERE state IN ({placeholders}) ORDER BY created_at",
                ACTIVE_STATES,
            ).fetchall()
        return [row["id"] for row in rows]

    def redirect_candidates(self, *, older_than: int) -> list[Capture]:
        """Replies that went unclaimed long enough to belong in the notification queue."""
        with self._db.read() as conn:
            rows = conn.execute(
                """
                SELECT * FROM captures
                 WHERE state='reply_ready'
                   AND redirected=0
                   AND reply_downloaded_at IS NULL
                   AND reply_ready_at IS NOT NULL
                   AND reply_ready_at < ?
                 ORDER BY reply_ready_at
                """,
                (older_than,),
            ).fetchall()
        return [_to_capture(row) for row in rows]

    def mark_redirected(self, capture_id: str) -> None:
        with self._db.write() as conn:
            conn.execute(
                "UPDATE captures SET redirected=1, updated_at=? WHERE id=?",
                (self._clock(), capture_id),
            )

    def ingestion_backlog(self) -> list[Capture]:
        """Notes the device considers finished but the agent has not accepted yet."""
        with self._db.read() as conn:
            rows = conn.execute(
                "SELECT * FROM captures WHERE state='done' AND error=? ORDER BY created_at",
                (INGEST_FAILED,),
            ).fetchall()
        return [_to_capture(row) for row in rows]

    def reply_latencies(self, limit: int = 50) -> list[ReplyLatency]:
        with self._db.read() as conn:
            rows = conn.execute(
                """
                SELECT id, created_at, reply_ready_at, reply_downloaded_at
                  FROM captures
                 WHERE reply_ready_at IS NOT NULL
                 ORDER BY reply_ready_at DESC
                 LIMIT ?
                """,
                (limit,),
            ).fetchall()
        return [
            ReplyLatency(
                capture_id=row["id"],
                pipeline_seconds=row["reply_ready_at"] - row["created_at"],
                download_seconds=(
                    row["reply_downloaded_at"] - row["reply_ready_at"]
                    if row["reply_downloaded_at"] is not None
                    else None
                ),
            )
            for row in rows
        ]

    def conversation_history(self, conversation_id: str, *, exclude_id: str) -> list[tuple[str, str]]:
        """Completed (user, assistant) exchanges in this conversation, oldest first."""
        with self._db.read() as conn:
            rows = conn.execute(
                """
                SELECT transcript, reply_text FROM captures
                 WHERE conversation_id=?
                   AND id != ?
                   AND transcript IS NOT NULL
                   AND reply_text IS NOT NULL
                 ORDER BY created_at
                """,
                (conversation_id, exclude_id),
            ).fetchall()
        return [(row["transcript"], row["reply_text"]) for row in rows]
