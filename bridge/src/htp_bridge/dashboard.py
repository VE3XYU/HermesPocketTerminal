from __future__ import annotations

import hashlib
import json
import time
from dataclasses import dataclass, asdict
from typing import Any, Callable

from htp_bridge.config import DashboardConfig
from htp_bridge.db import Database

KNOWN_STYLES = {"bold", "dim"}


@dataclass(frozen=True)
class DashboardItem:
    id: str
    text: str
    done: bool
    style: str | None = None


@dataclass(frozen=True)
class DashboardSnapshot:
    rev: str
    title: str
    items: list[DashboardItem]
    updated_at: int


def _revision(title: str, items: list[DashboardItem]) -> str:
    """Content hash. Identical content must yield an identical revision so the
    device can skip an e-paper refresh."""
    payload = json.dumps(
        {"title": title, "items": [asdict(item) for item in items]},
        sort_keys=True,
        separators=(",", ":"),
    )
    return hashlib.sha256(payload.encode()).hexdigest()[:8]


class DashboardStore:
    def __init__(
        self,
        db: Database,
        config: DashboardConfig,
        clock: Callable[[], int] = lambda: int(time.time()),
    ) -> None:
        self._db = db
        self._config = config
        self._clock = clock

    def publish(self, title: str, items: list[dict[str, Any]]) -> str:
        normalized = self._normalize(items)
        rev = _revision(title, normalized)
        payload = json.dumps([asdict(item) for item in normalized])
        with self._db.write() as conn:
            conn.execute(
                """
                INSERT INTO dashboard (id, rev, title, items_json, updated_at) VALUES (1,?,?,?,?)
                ON CONFLICT(id) DO UPDATE SET
                    rev=excluded.rev,
                    title=excluded.title,
                    items_json=excluded.items_json,
                    updated_at=excluded.updated_at
                """,
                (rev, title, payload, self._clock()),
            )
        return rev

    def current(self) -> DashboardSnapshot | None:
        with self._db.read() as conn:
            row = conn.execute("SELECT * FROM dashboard WHERE id=1").fetchone()
        if row is None:
            return None
        items = [DashboardItem(**entry) for entry in json.loads(row["items_json"])]
        return DashboardSnapshot(
            rev=row["rev"], title=row["title"], items=items, updated_at=row["updated_at"]
        )

    def complete(self, item_id: str) -> str | None:
        """Mark an item done in the snapshot. Returns the new revision, or None
        if there is no such item. The agent's next publish overrides this."""
        snapshot = self.current()
        if snapshot is None:
            return None
        if not any(item.id == item_id for item in snapshot.items):
            return None
        updated = [
            DashboardItem(id=i.id, text=i.text, done=True, style=i.style) if i.id == item_id else i
            for i in snapshot.items
        ]
        return self.publish(snapshot.title, [asdict(item) for item in updated])

    def _normalize(self, items: list[dict[str, Any]]) -> list[DashboardItem]:
        normalized: list[DashboardItem] = []
        for entry in items or []:
            if not isinstance(entry, dict):
                continue
            item_id = str(entry.get("id") or "").strip()
            text = str(entry.get("text") or "").strip()
            if not item_id or not text:
                continue
            if len(text) > self._config.max_text_chars:
                text = text[: self._config.max_text_chars - 1] + "…"
            style = entry.get("style")
            style = style if isinstance(style, str) and style in KNOWN_STYLES else None
            normalized.append(
                DashboardItem(
                    id=item_id,
                    text=text,
                    done=bool(entry.get("done", False)),
                    style=style,
                )
            )
            if len(normalized) >= self._config.max_items:
                break
        return normalized
