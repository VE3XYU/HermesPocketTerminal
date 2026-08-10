# Bridge Pipeline Timing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record per-stage durations (transcribe, agent, synthesize, save) for every capture the bridge processes, so the ~26 s conversation turnaround can be attributed to a stage.

**Architecture:** A new `timing.py` holds an in-memory `CaptureTimer` (context-manager stages, injected time sources) and a `TimingStore` that persists one row per capture to a new `capture_timings` table. `Pipeline` creates a timer per capture and writes the row in a `finally`, so failures record too. A `htp-timings` console script reads the table back as a per-stage summary.

**Tech Stack:** Python 3.11+, SQLite via the existing `Database` wrapper, pytest with `asyncio_mode = "auto"`.

## Global Constraints

Copied from the spec and the repo's binding rules. Every task's requirements implicitly include this section.

- Python 3.11+ (stdlib `tomllib`).
- **Time and ID generation are injected**, never called inline — no bare `time.time()`, `time.monotonic()`, `datetime.now()`, or `uuid4()` in business logic. Injection with a module-level default parameter (the pattern `clock: Callable[[], int] = lambda: int(time.time())` already uses) is the accepted form.
- Timestamps are **integer Unix epoch seconds**; durations are **integer milliseconds**.
- Tests never touch the network. Speech and agent clients are faked.
- No transcript or reply **text** in the log line or the timing table — lengths only.
- No change to the HTP wire protocol and no new device-visible fields.
- All **252** existing tests stay green after every task.
- Commit subjects use `feat(bridge): …` / `test(bridge): …` / `docs(bridge): …`.
- Run tests from `bridge/`. In this checkout the prepared environment is `.venv/bin/python`; a fresh worktree needs `python3.11 -m venv .venv && .venv/bin/pip install -e ".[dev]"` first. All commands below assume `bridge/` is the working directory.

---

### Task 1: `CaptureTimer` and `CaptureTiming`

The in-memory half of the feature: measures stages, formats the log line. No SQL, no pipeline.

**Files:**
- Create: `bridge/src/htp_bridge/timing.py`
- Test: `bridge/tests/test_timing.py`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `STAGES: tuple[str, ...] = ("transcribe", "agent", "synthesize", "save")`
  - `SIZES: tuple[str, ...] = ("audio_bytes", "transcript_chars", "reply_chars", "reply_bytes")`
  - `CaptureTiming(capture_id: str, kind: str, outcome: str, started_at: int, total_ms: int, stages: dict[str, int], sizes: dict[str, int])` — frozen dataclass with `as_log_line() -> str`
  - `CaptureTimer(capture_id: str, *, monotonic: Callable[[], float], clock: Callable[[], int])` with `stage(name) -> ContextManager[None]`, `note(**sizes: int | None) -> None`, `set_kind(kind: str) -> None`, `finish(outcome: str) -> CaptureTiming`
  - `log_timing(timing: CaptureTiming) -> None`

- [ ] **Step 1: Write the failing tests**

Create `bridge/tests/test_timing.py`:

```python
import pytest

from htp_bridge.timing import CaptureTimer, CaptureTiming


class ScriptedMonotonic:
    """Returns a fixed sequence, so durations are exact rather than approximate."""

    def __init__(self, values):
        self._values = list(values)

    def __call__(self):
        return self._values.pop(0)


def build_timer(values, *, now=1_000_000):
    return CaptureTimer("c-1", monotonic=ScriptedMonotonic(values), clock=lambda: now)


def test_stage_duration_is_exact():
    # construct=0.0, stage start=1.0, stage end=4.5, finish=10.0
    timer = build_timer([0.0, 1.0, 4.5, 10.0])
    with timer.stage("transcribe"):
        pass
    timing = timer.finish("ok")
    assert timing.stages == {"transcribe": 3500}
    assert timing.total_ms == 10000
    assert timing.started_at == 1_000_000


def test_stage_records_duration_when_body_raises():
    timer = build_timer([0.0, 1.0, 3.0, 5.0])
    with pytest.raises(RuntimeError):
        with timer.stage("agent"):
            raise RuntimeError("agent timed out")
    timing = timer.finish("agent_unavailable")
    assert timing.stages == {"agent": 2000}
    assert timing.outcome == "agent_unavailable"


def test_unknown_stage_name_is_rejected():
    timer = build_timer([0.0])
    with pytest.raises(ValueError):
        with timer.stage("transcribbe"):
            pass


def test_unknown_size_name_is_rejected():
    timer = build_timer([0.0])
    with pytest.raises(ValueError):
        timer.note(audio_byte=12)


def test_note_ignores_none_and_kind_defaults_to_unknown():
    timer = build_timer([0.0, 1.0])
    timer.note(audio_bytes=4096, transcript_chars=None)
    timing = timer.finish("ok")
    assert timing.sizes == {"audio_bytes": 4096}
    assert timing.kind == "unknown"


def test_log_line_omits_absent_stages_and_sizes():
    timing = CaptureTiming(
        capture_id="c-0a1f",
        kind="conversation",
        outcome="ok",
        started_at=1_000_000,
        total_ms=25840,
        stages={"transcribe": 3120, "agent": 19980, "synthesize": 2610, "save": 12},
        sizes={"audio_bytes": 131116, "transcript_chars": 42, "reply_chars": 88},
    )
    assert timing.as_log_line() == (
        "capture c-0a1f timing kind=conversation outcome=ok total=25840ms "
        "transcribe=3120ms agent=19980ms synthesize=2610ms save=12ms "
        "audio=131116B transcript=42c reply=88c"
    )


def test_log_line_with_only_a_failed_transcribe_stage():
    timing = CaptureTiming(
        capture_id="c-2",
        kind="unknown",
        outcome="transcription_failed",
        started_at=1_000_000,
        total_ms=30050,
        stages={"transcribe": 30000},
        sizes={"audio_bytes": 8},
    )
    assert timing.as_log_line() == (
        "capture c-2 timing kind=unknown outcome=transcription_failed "
        "total=30050ms transcribe=30000ms audio=8B"
    )
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `.venv/bin/python -m pytest tests/test_timing.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.timing'`

- [ ] **Step 3: Write the implementation**

Create `bridge/src/htp_bridge/timing.py`:

```python
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
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `.venv/bin/python -m pytest tests/test_timing.py -v`
Expected: PASS, 7 tests.

- [ ] **Step 5: Run the whole suite**

Run: `.venv/bin/python -m pytest tests/ -q`
Expected: `259 passed` (252 existing + 7 new).

- [ ] **Step 6: Commit**

```bash
git add src/htp_bridge/timing.py tests/test_timing.py
git commit -m "feat(bridge): add CaptureTimer for per-stage pipeline measurement"
```

---

### Task 2: `capture_timings` table and `TimingStore`

Persists the rows and reads them back as a summary. Still no pipeline changes.

**Files:**
- Modify: `bridge/src/htp_bridge/db.py` (append to `SCHEMA`)
- Modify: `bridge/src/htp_bridge/timing.py` (append store classes)
- Test: `bridge/tests/test_timing.py` (append)

**Interfaces:**
- Consumes: `CaptureTiming`, `STAGES`, `SIZES` from Task 1; `Database` from `htp_bridge.db`.
- Produces:
  - `StageSummary(kind: str, stage: str, count: int, min_ms: int, median_ms: int, p90_ms: int, max_ms: int)` — frozen dataclass
  - `TimingStore(db: Database)` with `record(timing) -> None`, `recent(limit: int = 20) -> list[CaptureTiming]`, `summary() -> list[StageSummary]`
  - `NullTimingStore()` — same three methods, no-ops

`summary()` reports the four stages **plus a `total` pseudo-stage**, so the readout shows whole-pipeline spread beside the parts. Percentiles use nearest-rank (`rank = ceil(p/100 × n)`, 1-indexed) so results are deterministic and testable.

- [ ] **Step 1: Write the failing tests**

Append to `bridge/tests/test_timing.py`:

```python
from htp_bridge.timing import NullTimingStore, TimingStore


def timing(capture_id, *, kind="conversation", outcome="ok", started_at=1_000_000,
           total_ms=1000, stages=None, sizes=None):
    return CaptureTiming(
        capture_id=capture_id,
        kind=kind,
        outcome=outcome,
        started_at=started_at,
        total_ms=total_ms,
        stages=stages if stages is not None else {"transcribe": 100, "agent": 800},
        sizes=sizes if sizes is not None else {"audio_bytes": 4096},
    )


def test_store_round_trips_a_timing(db):
    store = TimingStore(db)
    store.record(timing("c-1"))

    (loaded,) = store.recent()
    assert loaded == timing("c-1")


def test_record_is_idempotent_on_capture_id(db):
    store = TimingStore(db)
    store.record(timing("c-1", total_ms=1000))
    store.record(timing("c-1", total_ms=2000))

    rows = store.recent()
    assert len(rows) == 1
    assert rows[0].total_ms == 2000


def test_recent_returns_newest_first_and_honours_limit(db):
    store = TimingStore(db)
    for index in range(3):
        store.record(timing(f"c-{index}", started_at=1_000_000 + index))

    assert [row.capture_id for row in store.recent(limit=2)] == ["c-2", "c-1"]


def test_summary_uses_nearest_rank_percentiles(db):
    store = TimingStore(db)
    # agent samples: 100, 200, 300, 400, 500 -> p50=300, p90=500
    for index, agent_ms in enumerate([100, 200, 300, 400, 500]):
        store.record(
            timing(f"c-{index}", total_ms=agent_ms + 50, stages={"agent": agent_ms})
        )

    agent = next(s for s in store.summary() if s.stage == "agent")
    assert (agent.kind, agent.count) == ("conversation", 5)
    assert (agent.min_ms, agent.median_ms, agent.p90_ms, agent.max_ms) == (100, 300, 500, 500)


def test_summary_separates_kinds_and_includes_total(db):
    store = TimingStore(db)
    store.record(timing("c-1", kind="note", stages={"agent": 900}, total_ms=1000))
    store.record(timing("c-2", kind="conversation", stages={"agent": 100}, total_ms=200))

    pairs = {(s.kind, s.stage): s.median_ms for s in store.summary()}
    assert pairs[("note", "agent")] == 900
    assert pairs[("conversation", "agent")] == 100
    assert pairs[("note", "total")] == 1000


def test_summary_skips_stages_that_never_ran(db):
    store = TimingStore(db)
    store.record(timing("c-1", stages={"transcribe": 100}))

    assert {s.stage for s in store.summary()} == {"transcribe", "total"}


def test_null_store_records_nothing(db):
    store = NullTimingStore()
    store.record(timing("c-1"))
    assert store.recent() == []
    assert store.summary() == []
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `.venv/bin/python -m pytest tests/test_timing.py -v -k "store or summary"`
Expected: FAIL — `ImportError: cannot import name 'TimingStore'`

- [ ] **Step 3: Add the table**

In `bridge/src/htp_bridge/db.py`, append to the `SCHEMA` string, after the `device_status` table:

```sql
CREATE TABLE IF NOT EXISTS capture_timings (
    capture_id       TEXT PRIMARY KEY,
    kind             TEXT NOT NULL,
    outcome          TEXT NOT NULL,
    started_at       INTEGER NOT NULL,
    total_ms         INTEGER NOT NULL,
    transcribe_ms    INTEGER,
    agent_ms         INTEGER,
    synthesize_ms    INTEGER,
    save_ms          INTEGER,
    audio_bytes      INTEGER,
    transcript_chars INTEGER,
    reply_chars      INTEGER,
    reply_bytes      INTEGER
);
CREATE INDEX IF NOT EXISTS idx_capture_timings_started ON capture_timings(started_at);
```

A new table needs no migration: `executescript(SCHEMA)` creates it on the next start of the already-deployed database.

- [ ] **Step 4: Write the store**

Append to `bridge/src/htp_bridge/timing.py` (and add `import math` plus `from htp_bridge.db import Database` at the top):

```python
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
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `.venv/bin/python -m pytest tests/test_timing.py -v`
Expected: PASS, 14 tests.

- [ ] **Step 6: Run the whole suite**

Run: `.venv/bin/python -m pytest tests/ -q`
Expected: `266 passed`.

- [ ] **Step 7: Commit**

```bash
git add src/htp_bridge/db.py src/htp_bridge/timing.py tests/test_timing.py
git commit -m "feat(bridge): persist capture timings in a new capture_timings table"
```

---

### Task 3: Wire timing into the pipeline

Makes the feature live: real captures write rows, and the service builds a real store.

**Files:**
- Modify: `bridge/src/htp_bridge/pipeline.py`
- Modify: `bridge/src/htp_bridge/main.py:33-60` (`build_deps`)
- Test: `bridge/tests/test_pipeline.py` (append)

**Interfaces:**
- Consumes: `CaptureTimer`, `TimingStore`, `NullTimingStore`, `log_timing` from Tasks 1–2.
- Produces: `Pipeline(..., timings: TimingStore | None = None, monotonic: Callable[[], float] = time.monotonic)`. `process()` keeps its existing signature `async def process(self, capture_id: str) -> None`.

Key behaviours this task must produce:

- The timer is created **after** the existing early return, so a duplicate upload of an already-terminal capture writes no row.
- `process()` wraps its body in `try/finally`, so the row is written even when an exception escapes to `api._run_pipeline`'s safety net — and the exception still propagates.
- `outcome` is read from the capture row after the body: a note whose ingest failed lands as `outcome=ingest_failed`, because `_finish_note` writes that flag before calling the agent.
- A `TimingStore` that raises must not fail the capture.

- [ ] **Step 1: Write the failing tests**

Append to `bridge/tests/test_pipeline.py`:

```python
from htp_bridge.timing import TimingStore


class StepMonotonic:
    """Advances one second per call, so each stage measures exactly 1000 ms
    regardless of how many calls the surrounding code makes."""

    def __init__(self):
        self._calls = 0

    def __call__(self):
        value = float(self._calls)
        self._calls += 1
        return value


class ExplodingTimingStore:
    def record(self, timing):
        raise RuntimeError("disk full")


def build_timed(parts, fake_clock, *, speech, agent, timings):
    captures, storage, notifications = parts
    conversations = itertools.count(1)
    return Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=speech,
        agent=agent,
        salutation_prefixes=PREFIXES,
        clock=fake_clock,
        conversation_id_factory=lambda: f"v-{next(conversations)}",
        timings=timings,
        monotonic=StepMonotonic(),
    )


async def test_conversation_records_all_four_stages(parts, fake_clock, db):
    timings = TimingStore(db)
    speech = FakeSpeechProvider(transcripts={"c-1": "hey hermes what is on my list"})
    agent = FakeAgentClient(reply_text="Milk and bread.")
    upload(parts, "c-1")

    await build_timed(parts, fake_clock, speech=speech, agent=agent, timings=timings).process("c-1")

    (row,) = timings.recent()
    assert row.kind == "conversation"
    assert row.outcome == "ok"
    assert row.stages == {"transcribe": 1000, "agent": 1000, "synthesize": 1000, "save": 1000}
    assert row.sizes["transcript_chars"] == len("hey hermes what is on my list")
    assert row.sizes["reply_chars"] == len("Milk and bread.")
    assert row.sizes["audio_bytes"] == len(b"RIFFfake")
    assert row.sizes["reply_bytes"] == len(b"RIFF-fake-reply")
    assert row.total_ms >= sum(row.stages.values())


async def test_note_records_transcribe_and_agent_only(parts, fake_clock, db):
    timings = TimingStore(db)
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk to the shopping list"})
    agent = FakeAgentClient()
    upload(parts, "c-1")

    await build_timed(parts, fake_clock, speech=speech, agent=agent, timings=timings).process("c-1")

    (row,) = timings.recent()
    assert row.kind == "note"
    assert row.outcome == "ok"
    assert row.stages == {"transcribe": 1000, "agent": 1000}


async def test_failed_transcription_records_a_partial_row(parts, fake_clock, db):
    timings = TimingStore(db)
    speech = FakeSpeechProvider(transcribe_error=SpeechError("boom"))
    agent = FakeAgentClient()
    upload(parts, "c-1")

    await build_timed(parts, fake_clock, speech=speech, agent=agent, timings=timings).process("c-1")

    (row,) = timings.recent()
    assert row.kind == "unknown"
    assert row.outcome == "transcription_failed"
    assert row.stages == {"transcribe": 1000}


async def test_failed_ingest_records_ingest_failed_outcome(parts, fake_clock, db):
    timings = TimingStore(db)
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk to the shopping list"})
    agent = FakeAgentClient(error=AgentError("down"))
    upload(parts, "c-1")

    await build_timed(parts, fake_clock, speech=speech, agent=agent, timings=timings).process("c-1")

    (row,) = timings.recent()
    assert row.kind == "note"
    assert row.outcome == "ingest_failed"


async def test_already_terminal_capture_writes_no_timing_row(parts, fake_clock, db):
    captures, _, _ = parts
    timings = TimingStore(db)
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk to the shopping list"})
    agent = FakeAgentClient()
    upload(parts, "c-1")
    captures.set_state("c-1", "done")

    await build_timed(parts, fake_clock, speech=speech, agent=agent, timings=timings).process("c-1")

    assert timings.recent() == []


async def test_timing_write_failure_does_not_fail_the_capture(parts, fake_clock):
    captures, _, _ = parts
    speech = FakeSpeechProvider(transcripts={"c-1": "Add milk to the shopping list"})
    agent = FakeAgentClient()
    upload(parts, "c-1")

    pipeline = build_timed(
        parts, fake_clock, speech=speech, agent=agent, timings=ExplodingTimingStore()
    )
    await pipeline.process("c-1")

    assert captures.get("c-1").state == "done"
    assert captures.get("c-1").error is None
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `.venv/bin/python -m pytest tests/test_pipeline.py -v -k "records or timing or terminal"`
Expected: FAIL — `TypeError: Pipeline.__init__() got an unexpected keyword argument 'timings'`

- [ ] **Step 3: Rewrite `process()` and thread the timer through**

In `bridge/src/htp_bridge/pipeline.py`, add imports:

```python
from htp_bridge.captures import INGEST_FAILED, TERMINAL_STATES, Capture, CaptureStore
from htp_bridge.timing import CaptureTimer, NullTimingStore, TimingStore, log_timing
```

Add a module-level helper:

```python
def _file_size(path) -> int | None:
    """Upload size for the timing row. Absent audio is a real case (the crash
    window resume() documents), so a missing file records nothing rather than
    raising inside instrumentation."""
    try:
        return path.stat().st_size
    except OSError:
        return None
```

Extend `__init__` with two parameters (keep every existing one unchanged):

```python
        timings: TimingStore | None = None,
        monotonic: Callable[[], float] = time.monotonic,
```

and in the body:

```python
        self._timings = timings or NullTimingStore()
        self._monotonic = monotonic
```

Replace `process()` with a wrapper plus the renamed body:

```python
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
```

Add `timer: CaptureTimer` as a trailing parameter to `_finish_note` and `_answer`, and wrap their calls:

In `_finish_note`, replace the `await self._agent.ingest(...)` line with:

```python
            with timer.stage("agent"):
                await self._agent.ingest(transcript, recorded_at)
```

In `_answer`, wrap the three operations:

```python
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
```

- [ ] **Step 4: Run the pipeline tests**

Run: `.venv/bin/python -m pytest tests/test_pipeline.py -v`
Expected: PASS — the six new tests plus every pre-existing one.

- [ ] **Step 5: Build the real store in `build_deps`**

In `bridge/src/htp_bridge/main.py`, add `from htp_bridge.timing import TimingStore` and, inside `build_deps`, construct it from the same `db` and pass it in:

```python
    timings = TimingStore(db)
    pipeline = Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=build_speech_provider(config.speech),
        agent=agent,
        salutation_prefixes=config.salutations.prefixes,
        timings=timings,
    )
```

- [ ] **Step 6: Test that the assembled service records timings**

Append to `bridge/tests/test_main.py`:

```python
def test_build_deps_gives_the_pipeline_a_real_timing_store(config_path):
    from htp_bridge.timing import NullTimingStore, TimingStore

    deps, _ = build_deps(load_config(config_path))
    assert isinstance(deps.pipeline._timings, TimingStore)
    assert not isinstance(deps.pipeline._timings, NullTimingStore)
```

`config_path` is the existing fixture in that file, and `load_config` is already
imported there — reuse both rather than building a second `Config` by hand.

- [ ] **Step 7: Run the whole suite**

Run: `.venv/bin/python -m pytest tests/ -q`
Expected: `273 passed`.

- [ ] **Step 8: Commit**

```bash
git add src/htp_bridge/pipeline.py src/htp_bridge/main.py tests/test_pipeline.py tests/test_main.py
git commit -m "feat(bridge): record per-stage timings for every processed capture"
```

---

### Task 4: `htp-timings` readout

Turns the stored rows into something readable, including the two latencies the `captures` table already knows.

**Files:**
- Create: `bridge/src/htp_bridge/timing_report.py`
- Create: `bridge/tests/test_timing_report.py`
- Modify: `bridge/src/htp_bridge/captures.py` (add `ReplyLatency` and `reply_latencies`)
- Modify: `bridge/pyproject.toml` (`[project.scripts]`)
- Modify: `bridge/README.md`
- Test: `bridge/tests/test_captures.py` (append)

**Interfaces:**
- Consumes: `TimingStore.summary()`, `TimingStore.recent()`, `StageSummary`, `CaptureTiming`.
- Produces:
  - `ReplyLatency(capture_id: str, pipeline_seconds: int, download_seconds: int | None)` in `captures.py`
  - `CaptureStore.reply_latencies(limit: int = 50) -> list[ReplyLatency]`
  - `render_summary(summaries: list[StageSummary], latencies: list[ReplyLatency]) -> str`
  - `render_recent(timings: list[CaptureTiming]) -> str`
  - `main(argv: list[str] | None = None) -> int`

- [ ] **Step 1: Write the failing test for `reply_latencies`**

Append to `bridge/tests/test_captures.py`:

```python
def test_reply_latencies_reports_pipeline_and_download_gaps(db, fake_clock):
    s = store(db, fake_clock)
    s.create(capture_id="c-1", device_id="pocket-01", recorded_at=1, conversation_id=None)
    fake_clock.advance(20)
    s.set_reply("c-1", "Milk and bread.")
    fake_clock.advance(6)
    s.mark_downloaded("c-1")

    s.create(capture_id="c-2", device_id="pocket-01", recorded_at=1, conversation_id=None)
    fake_clock.advance(5)
    s.set_reply("c-2", "Still thinking.")

    latencies = {row.capture_id: row for row in s.reply_latencies()}
    assert latencies["c-1"].pipeline_seconds == 20
    assert latencies["c-1"].download_seconds == 6
    assert latencies["c-2"].download_seconds is None
```

`store(db, fake_clock)` is the existing module-level helper in that file, which
already imports `CaptureStore` — use it rather than constructing a store inline.

- [ ] **Step 2: Run it to verify it fails**

Run: `.venv/bin/python -m pytest tests/test_captures.py -k reply_latencies -v`
Expected: FAIL — `AttributeError: 'CaptureStore' object has no attribute 'reply_latencies'`

- [ ] **Step 3: Implement `reply_latencies`**

In `bridge/src/htp_bridge/captures.py`, add the dataclass beside `Capture`:

```python
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
```

and the method on `CaptureStore`:

```python
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
```

- [ ] **Step 4: Run it to verify it passes**

Run: `.venv/bin/python -m pytest tests/test_captures.py -k reply_latencies -v`
Expected: PASS

- [ ] **Step 5: Write the failing tests for the renderers**

Create `bridge/tests/test_timing_report.py`:

```python
from htp_bridge.captures import ReplyLatency
from htp_bridge.timing import CaptureTiming, StageSummary
from htp_bridge.timing_report import render_recent, render_summary


def test_render_summary_groups_by_kind_and_shows_device_lag():
    summaries = [
        StageSummary("conversation", "agent", 3, 100, 200, 300, 300),
        StageSummary("conversation", "total", 3, 150, 250, 350, 350),
    ]
    latencies = [
        ReplyLatency("c-1", pipeline_seconds=20, download_seconds=6),
        ReplyLatency("c-2", pipeline_seconds=24, download_seconds=None),
    ]

    out = render_summary(summaries, latencies)

    assert "conversation" in out
    assert "agent" in out
    assert "300" in out
    # device lag reported from the one sample that has it
    assert "6" in out
    assert "2 conversation(s)" in out


def test_render_summary_with_no_data_says_so():
    assert "no timings recorded" in render_summary([], []).lower()


def test_render_recent_lists_one_line_per_capture():
    rows = [
        CaptureTiming("c-1", "conversation", "ok", 1_000_000, 25840,
                      {"transcribe": 3120, "agent": 19980}, {"audio_bytes": 131116}),
        CaptureTiming("c-2", "note", "ingest_failed", 1_000_100, 900,
                      {"transcribe": 400}, {}),
    ]

    out = render_recent(rows)

    assert out.count("\n") >= 1
    assert "c-1" in out and "c-2" in out
    assert "ingest_failed" in out
```

- [ ] **Step 6: Run them to verify they fail**

Run: `.venv/bin/python -m pytest tests/test_timing_report.py -v`
Expected: FAIL — `ModuleNotFoundError: No module named 'htp_bridge.timing_report'`

- [ ] **Step 7: Write the report**

Create `bridge/src/htp_bridge/timing_report.py`:

```python
from __future__ import annotations

import argparse
from pathlib import Path

from htp_bridge.captures import CaptureStore, ReplyLatency
from htp_bridge.config import load_config
from htp_bridge.db import Database
from htp_bridge.timing import STAGES, CaptureTiming, StageSummary, TimingStore

_HEADER = f"{'kind':<13} {'stage':<11} {'n':>4} {'min':>8} {'med':>8} {'p90':>8} {'max':>8}"


def render_summary(summaries: list[StageSummary], latencies: list[ReplyLatency]) -> str:
    if not summaries:
        return "No timings recorded yet."

    lines = [_HEADER, "-" * len(_HEADER)]
    for row in summaries:
        lines.append(
            f"{row.kind:<13} {row.stage:<11} {row.count:>4} "
            f"{row.min_ms:>7}ms {row.median_ms:>7}ms {row.p90_ms:>7}ms {row.max_ms:>7}ms"
        )

    if latencies:
        pipeline = sorted(row.pipeline_seconds for row in latencies)
        downloads = sorted(
            row.download_seconds for row in latencies if row.download_seconds is not None
        )
        lines.append("")
        lines.append(f"Reply latencies over {len(latencies)} conversation(s), 1s resolution:")
        lines.append(
            f"  upload -> reply_ready   min {pipeline[0]}s  max {pipeline[-1]}s"
        )
        if downloads:
            lines.append(
                f"  reply_ready -> fetched  min {downloads[0]}s  max {downloads[-1]}s"
                "   (device side; bridge work cannot shorten this)"
            )
        else:
            lines.append("  reply_ready -> fetched  no reply has been downloaded yet")
    return "\n".join(lines)


def render_recent(timings: list[CaptureTiming]) -> str:
    if not timings:
        return "No timings recorded yet."
    lines = []
    for row in timings:
        stages = " ".join(
            f"{name}={row.stages[name]}ms" for name in STAGES if name in row.stages
        )
        lines.append(
            f"{row.started_at} {row.capture_id:<16} {row.kind:<12} "
            f"{row.outcome:<22} total={row.total_ms}ms {stages}"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="htp-timings", description="Per-stage timings for bridge captures."
    )
    parser.add_argument("--db", default=None, help="path to the bridge database")
    parser.add_argument(
        "--config",
        default="/etc/htp-bridge/config.toml",
        help="read the database path from this config (ignored when --db is given)",
    )
    parser.add_argument(
        "--recent", type=int, default=0, help="list the N most recent captures instead"
    )
    args = parser.parse_args(argv)

    db_path = Path(args.db) if args.db else load_config(Path(args.config)).storage.db_path
    db = Database(db_path)
    try:
        timings = TimingStore(db)
        if args.recent:
            print(render_recent(timings.recent(limit=args.recent)))
        else:
            print(render_summary(timings.summary(), CaptureStore(db).reply_latencies()))
    finally:
        db.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 8: Run them to verify they pass**

Run: `.venv/bin/python -m pytest tests/test_timing_report.py -v`
Expected: PASS, 3 tests.

- [ ] **Step 9: Register the console script**

In `bridge/pyproject.toml`, under `[project.scripts]`:

```toml
[project.scripts]
htp-bridge = "htp_bridge.main:main"
htp-timings = "htp_bridge.timing_report:main"
```

- [ ] **Step 10: Document the command**

Add a short section to `bridge/README.md` covering: what `capture_timings` records, the summary and `--recent` modes, and the fact that on the deployed host the command runs as the service user against the database path directly, because the config file is mode 600 and the state directory belongs to that user:

```bash
sudo -u htp /opt/htp-bridge/venv/bin/htp-timings --db /var/lib/htp-bridge/htp.db
```

Note in that section that the table needs no migration — it appears on the next service start — and that the log line is greppable with the fixed token `timing` on the `htp_bridge.timing` logger.

- [ ] **Step 11: Run the whole suite**

Run: `.venv/bin/python -m pytest tests/ -q`
Expected: `293 passed` (278 from the original four tasks, plus 15 added during the
post-implementation review pass that fixed outcome tracking, the failed/ok
split in `summary()`, and the readout's edge cases).

- [ ] **Step 12: Verify the script is installed and runs**

```bash
.venv/bin/pip install -e ".[dev]" >/dev/null && .venv/bin/htp-timings --db /tmp/nonexistent-timing-check.db
```
Expected (post-review-fix; see the design doc's changelog): prints
`no such database: /tmp/nonexistent-timing-check.db` to stderr and exits 1,
without creating the file. The `Database` constructor would otherwise create
an empty database silently, making a typo'd `--db` path indistinguishable
from a real, empty one -- caught in the post-implementation review and fixed
before merge.

- [ ] **Step 13: Commit**

```bash
git add src/htp_bridge/timing_report.py src/htp_bridge/captures.py pyproject.toml \
        README.md tests/test_timing_report.py tests/test_captures.py
git commit -m "feat(bridge): add htp-timings readout for stage and reply latencies"
```

---

## Done criteria

- `.venv/bin/python -m pytest tests/ -q` reports 293 passed.
- A conversation processed through `--mock` or a real capture writes one `capture_timings` row and one `htp_bridge.timing` log line.
- `htp-timings` prints a per-stage summary plus the two reply latencies.
- Nothing in `git diff main` touches the HTP wire protocol, the device firmware, or the response shape of any endpoint.

## After the plan

Deployment is the established sequence, and its ordering trap still applies: pull the shared checkout **before** running the install script, since the script reads the bridge source from that path — reinstalling first silently redeploys the old code. The service restart afterwards is required; the script's `enable --now` is a no-op on an already-running unit. The new table appears on that restart with no migration step.

Then collect real numbers from a handful of conversations before opening the optimization cycle. The point of this work is to make that next design data-driven.
