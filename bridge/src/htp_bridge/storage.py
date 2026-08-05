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


def _repaired_wav(data: bytes) -> bytes:
    """Rewrite streaming placeholder sizes to describe the bytes actually present.

    Speech providers synthesize as a stream and cannot know the length up front,
    so they emit 0xFFFFFFFF for the RIFF and data sizes (OpenAI /audio/speech
    does exactly this). The device plays a reply by reading its WAV header, so
    the bridge must not hand it a file that claims to be 4 GiB. Anything that is
    not a RIFF/WAVE container is passed through untouched.
    """
    if len(data) < 12 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        return data

    index = 12
    while index + 8 <= len(data):
        chunk_id = data[index : index + 4]
        declared = int.from_bytes(data[index + 4 : index + 8], "little")
        body = index + 8
        if chunk_id == b"data":
            actual = len(data) - body
            if declared == actual and int.from_bytes(data[4:8], "little") == len(data) - 8:
                return data
            repaired = bytearray(data)
            repaired[4:8] = (len(data) - 8).to_bytes(4, "little")
            repaired[index + 4 : index + 8] = actual.to_bytes(4, "little")
            return bytes(repaired)
        if declared > len(data) - body:
            return data  # unwalkable header; leave it alone rather than corrupt it
        index = body + declared + (declared % 2)
    return data


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
        temporary.write_bytes(_repaired_wav(data))
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
