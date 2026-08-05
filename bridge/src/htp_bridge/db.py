from __future__ import annotations

import sqlite3
import threading
from contextlib import contextmanager
from pathlib import Path

SCHEMA = """
CREATE TABLE IF NOT EXISTS captures (
    id                   TEXT PRIMARY KEY,
    device_id            TEXT NOT NULL,
    state                TEXT NOT NULL,
    transcript           TEXT,
    error                TEXT,
    conversation_id      TEXT,
    reply_text           TEXT,
    recorded_at          INTEGER,
    created_at           INTEGER NOT NULL,
    updated_at           INTEGER NOT NULL,
    reply_ready_at       INTEGER,
    reply_downloaded_at  INTEGER,
    redirected           INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_captures_state ON captures(state);
CREATE INDEX IF NOT EXISTS idx_captures_conversation ON captures(conversation_id, created_at);

CREATE TABLE IF NOT EXISTS dashboard (
    id         INTEGER PRIMARY KEY CHECK (id = 1),
    rev        TEXT NOT NULL,
    title      TEXT NOT NULL,
    items_json TEXT NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS notifications (
    id         TEXT PRIMARY KEY,
    text       TEXT NOT NULL,
    priority   TEXT NOT NULL,
    created_at INTEGER NOT NULL,
    acked_at   INTEGER
);
CREATE INDEX IF NOT EXISTS idx_notifications_acked ON notifications(acked_at, created_at);

CREATE TABLE IF NOT EXISTS device_status (
    device_id TEXT PRIMARY KEY,
    battery   INTEGER,
    last_seen INTEGER
);
"""


class Database:
    """A single SQLite connection guarded by a lock.

    The bridge is low-traffic and every statement is short, so one serialized
    connection is simpler and more predictable than a pool. WAL mode keeps
    committed state durable across crashes, which is what pipeline resume
    depends on.
    """

    def __init__(self, path: Path) -> None:
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(str(path), check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
        self._conn.executescript(SCHEMA)
        self._conn.commit()

    @contextmanager
    def write(self):
        with self._lock:
            try:
                yield self._conn
                self._conn.commit()
            except Exception:
                self._conn.rollback()
                raise

    @contextmanager
    def read(self):
        with self._lock:
            try:
                self._conn.execute("PRAGMA query_only=ON")
                yield self._conn
            finally:
                self._conn.execute("PRAGMA query_only=OFF")

    def close(self) -> None:
        with self._lock:
            self._conn.close()
