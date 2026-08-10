from __future__ import annotations

import logging
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Callable, Iterator

log = logging.getLogger("htp_bridge.timing")

STAGES = ("transcribe", "agent", "synthesize", "save")
SIZES = ("audio_bytes", "transcript_chars", "reply_chars", "reply_bytes")

# How each size appears in the log line: label, unit suffix.
_SIZE_LABELS = {
    "audio_bytes": ("audio", "B"),
    "transcript_chars": ("transcript", "c"),
    "reply_chars": ("reply", "c"),
    "reply_bytes": ("reply_wav", "B"),
}


def _ms(seconds: float) -> int:
    return int(round(seconds * 1000))


@dataclass(frozen=True)
class CaptureTiming:
    """One capture's measured pipeline run.

    `stages` and `sizes` hold only the keys that were actually recorded, so a
    capture that died in transcription carries one stage rather than four zeros.
    """

    capture_id: str
    kind: str
    outcome: str
    started_at: int
    total_ms: int
    stages: dict[str, int]
    sizes: dict[str, int]

    def as_log_line(self) -> str:
        parts = [
            f"capture {self.capture_id} timing",
            f"kind={self.kind}",
            f"outcome={self.outcome}",
            f"total={self.total_ms}ms",
        ]
        parts += [f"{name}={self.stages[name]}ms" for name in STAGES if name in self.stages]
        for name in SIZES:
            if name in self.sizes:
                label, unit = _SIZE_LABELS[name]
                parts.append(f"{label}={self.sizes[name]}{unit}")
        return " ".join(parts)


class CaptureTimer:
    """Accumulates one capture's stage durations.

    Both time sources are injected: `clock` stamps the row for ordering, and
    `monotonic` measures elapsed time immune to wall-clock adjustment. A scripted
    fake makes the durations exactly assertable in tests.
    """

    def __init__(
        self,
        capture_id: str,
        *,
        monotonic: Callable[[], float],
        clock: Callable[[], int],
    ) -> None:
        self._capture_id = capture_id
        self._monotonic = monotonic
        self._started_at = clock()
        self._start = monotonic()
        self._stages: dict[str, int] = {}
        self._sizes: dict[str, int] = {}
        self._kind = "unknown"

    @contextmanager
    def stage(self, name: str) -> Iterator[None]:
        """Time a stage, recording in a `finally` so a failure is measured too.

        A stage that times out is the most informative sample available and it
        exists only on the exception path.
        """
        if name not in STAGES:
            raise ValueError(f"unknown stage '{name}'")
        start = self._monotonic()
        try:
            yield
        finally:
            self._stages[name] = _ms(self._monotonic() - start)

    def note(self, **sizes: int | None) -> None:
        """Attach size correlates. A duration is not comparable across captures
        without them: synthesis scales with reply length, transcription with audio
        length."""
        for name, value in sizes.items():
            if name not in SIZES:
                raise ValueError(f"unknown size '{name}'")
            if value is not None:
                self._sizes[name] = int(value)

    def set_kind(self, kind: str) -> None:
        self._kind = kind

    def finish(self, outcome: str) -> CaptureTiming:
        return CaptureTiming(
            capture_id=self._capture_id,
            kind=self._kind,
            outcome=outcome,
            started_at=self._started_at,
            total_ms=_ms(self._monotonic() - self._start),
            stages=dict(self._stages),
            sizes=dict(self._sizes),
        )


def log_timing(timing: CaptureTiming) -> None:
    log.info("%s", timing.as_log_line())
