from __future__ import annotations

import os
import re
import time
from pathlib import Path
from typing import Callable

from htp_bridge.config import StorageConfig

_ID_PATTERN = re.compile(r"^[A-Za-z0-9_-]{1,128}\Z")


def is_valid_capture_id(value: str) -> bool:
    """Capture IDs become filenames, so restrict them to a safe alphabet."""
    return bool(_ID_PATTERN.match(value or ""))


class AudioStorage:
    def __init__(
        self,
        config: StorageConfig,
        clock: Callable[[], int] = lambda: int(time.time()),
    ) -> None:
        self._uploads = Path(config.audio_dir) / "uploads"
        self._replies = Path(config.audio_dir) / "replies"
        self._uploads.mkdir(parents=True, exist_ok=True)
        self._replies.mkdir(parents=True, exist_ok=True)
        self._upload_retention = config.upload_retention_days * 86400
        self._reply_retention = config.reply_retention_days * 86400
        self._clock = clock

    def upload_path(self, capture_id: str) -> Path:
        return self._uploads / f"{capture_id}.wav"

    def reply_path(self, capture_id: str) -> Path:
        return self._replies / f"{capture_id}.wav"

    def save_upload(self, capture_id: str, data: bytes) -> Path:
        path = self.upload_path(capture_id)
        temporary = path.with_suffix(".wav.part")
        temporary.write_bytes(data)
        temporary.replace(path)
        self._stamp(path)
        return path

    def save_reply(self, capture_id: str, data: bytes) -> Path:
        path = self.reply_path(capture_id)
        temporary = path.with_suffix(".wav.part")
        temporary.write_bytes(data)
        temporary.replace(path)
        self._stamp(path)
        return path

    def has_reply(self, capture_id: str) -> bool:
        return self.reply_path(capture_id).exists()

    def prune(self) -> int:
        """Delete audio past its retention window. Returns the number removed."""
        now = self._clock()
        removed = 0
        for directory, retention in ((self._uploads, self._upload_retention), (self._replies, self._reply_retention)):
            for path in directory.glob("*.wav"):
                if now - int(path.stat().st_mtime) > retention:
                    path.unlink(missing_ok=True)
                    removed += 1
        return removed

    def _stamp(self, path: Path) -> None:
        """Set mtime from the injected clock so retention is testable."""
        now = self._clock()
        os.utime(path, (now, now))
