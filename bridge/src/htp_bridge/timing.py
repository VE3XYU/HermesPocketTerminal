from __future__ import annotations

import logging
import math
from contextlib import contextmanager
from dataclasses import dataclass
from typing import Callable, Iterator

from htp_bridge.db import Database

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


@dataclass(frozen=True)
class StageSummary:
    kind: str
    stage: str
    count: int
    min_ms: int
    median_ms: int
    p90_ms: int
    max_ms: int


def _percentile(sorted_values: list[int], percent: int) -> int:
    """Nearest-rank percentile: deterministic, no interpolation, no float drift."""
    rank = math.ceil(percent / 100 * len(sorted_values))
    return sorted_values[max(rank, 1) - 1]


def _to_timing(row) -> CaptureTiming:
    return CaptureTiming(
        capture_id=row["capture_id"],
        kind=row["kind"],
        outcome=row["outcome"],
        started_at=row["started_at"],
        total_ms=row["total_ms"],
        stages={name: row[f"{name}_ms"] for name in STAGES if row[f"{name}_ms"] is not None},
        sizes={name: row[name] for name in SIZES if row[name] is not None},
    )


class TimingStore:
    """Owns the capture_timings table and nothing else."""

    def __init__(self, db: Database) -> None:
        self._db = db

    def record(self, timing: CaptureTiming) -> None:
        with self._db.write() as conn:
            conn.execute(
                """
                INSERT OR REPLACE INTO capture_timings
                    (capture_id, kind, outcome, started_at, total_ms,
                     transcribe_ms, agent_ms, synthesize_ms, save_ms,
                     audio_bytes, transcript_chars, reply_chars, reply_bytes)
                VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)
                """,
                (
                    timing.capture_id,
                    timing.kind,
                    timing.outcome,
                    timing.started_at,
                    timing.total_ms,
                    *(timing.stages.get(name) for name in STAGES),
                    *(timing.sizes.get(name) for name in SIZES),
                ),
            )

    def recent(self, limit: int = 20) -> list[CaptureTiming]:
        with self._db.read() as conn:
            rows = conn.execute(
                """
                SELECT * FROM capture_timings
                 ORDER BY started_at DESC, capture_id DESC
                 LIMIT ?
                """,
                (limit,),
            ).fetchall()
        return [_to_timing(row) for row in rows]

    def summary(self) -> list[StageSummary]:
        with self._db.read() as conn:
            rows = conn.execute("SELECT * FROM capture_timings").fetchall()

        samples: dict[tuple[str, str], list[int]] = {}
        for row in rows:
            for stage in (*STAGES, "total"):
                value = row[f"{stage}_ms"]
                if value is not None:
                    samples.setdefault((row["kind"], stage), []).append(value)

        summaries = []
        for (kind, stage), values in sorted(samples.items()):
            values.sort()
            summaries.append(
                StageSummary(
                    kind=kind,
                    stage=stage,
                    count=len(values),
                    min_ms=values[0],
                    median_ms=_percentile(values, 50),
                    p90_ms=_percentile(values, 90),
                    max_ms=values[-1],
                )
            )
        return summaries


class NullTimingStore:
    """Default for a Pipeline built without persistence, so the pipeline has one
    code path instead of `if self._timings` branches."""

    def record(self, timing: CaptureTiming) -> None:
        return None

    def recent(self, limit: int = 20) -> list[CaptureTiming]:
        return []

    def summary(self) -> list[StageSummary]:
        return []
