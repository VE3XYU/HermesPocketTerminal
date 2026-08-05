from __future__ import annotations

import time
import uuid
from dataclasses import dataclass
from typing import Callable

from htp_bridge.db import Database

VALID_PRIORITIES = {"normal", "urgent"}


@dataclass(frozen=True)
class Notification:
    id: str
    text: str
    priority: str
    created_at: int


def _default_id() -> str:
    return f"n-{uuid.uuid4().hex[:8]}"


class NotificationStore:
    def __init__(
        self,
        db: Database,
        clock: Callable[[], int] = lambda: int(time.time()),
        id_factory: Callable[[], str] = _default_id,
    ) -> None:
        self._db = db
        self._clock = clock
        self._id_factory = id_factory

    def enqueue(self, text: str, priority: str = "normal") -> str:
        if priority not in VALID_PRIORITIES:
            priority = "normal"
        notification_id = self._id_factory()
        with self._db.write() as conn:
            conn.execute(
                "INSERT INTO notifications (id, text, priority, created_at) VALUES (?,?,?,?)",
                (notification_id, text, priority, self._clock()),
            )
        return notification_id

    def pending(self) -> list[Notification]:
        """Unacknowledged notifications, oldest first. Fetching does not consume:
        delivery is confirmed by an explicit ack from the device."""
        with self._db.read() as conn:
            rows = conn.execute(
                "SELECT * FROM notifications WHERE acked_at IS NULL ORDER BY created_at, id"
            ).fetchall()
        return [
            Notification(r["id"], r["text"], r["priority"], r["created_at"]) for r in rows
        ]

    def ack(self, ids: list[str]) -> int:
        if not ids:
            return 0
        placeholders = ",".join("?" for _ in ids)
        with self._db.write() as conn:
            cursor = conn.execute(
                f"UPDATE notifications SET acked_at=? WHERE acked_at IS NULL AND id IN ({placeholders})",
                (self._clock(), *ids),
            )
            return cursor.rowcount
