# Bridge Pipeline Timing — Design

**Date:** 2026-08-09
**Status:** approved, not yet implemented
**Scope:** bridge only; no device firmware or wire-protocol change

## Problem

A conversation capture took roughly 26 seconds from upload to spoken reply during
bench checkpoint C6. Nothing in the pipeline is instrumented, so that number cannot
be attributed: transcription, the agent call, and speech synthesis are equally
plausible, and the agent is only *suspected* because notes took about 56 seconds
each during deployment.

This design ships observability, not optimization. Its success criterion is that
after a handful of real captures we can say which stage owns the time, and how much
of the user-visible wait is bridge work at all. The optimization is a separate cycle
with those numbers in hand.

## Non-goals

- No optimization. No streaming, no client reuse, no model changes.
- No HTP wire-protocol change and no new device-visible fields.
- No transcript or reply text in the log line or the timing table — lengths only.
- No instrumentation of `sweep_ingestion` or the upload request body read. The
  sweeps run on a 60-second timer nobody waits on, and body-receive time measures
  the device's Wi-Fi rather than the bridge.

## What gets measured

Both pipeline paths, since the note path costs almost nothing extra and directly
tests the agent-latency hypothesis:

| Path | Stages |
|---|---|
| Conversation | `transcribe`, `agent` (`converse`), `synthesize`, `save` |
| Note | `transcribe`, `agent` (`ingest`) |

Every stage is timed in a `finally`, so a stage that raises still records its
duration. A timeout is the most informative measurement available and it exists
only on the failure path.

Four size correlates accompany the durations: `audio_bytes`, `transcript_chars`,
`reply_chars`, `reply_bytes`. Synthesis time scales with reply length and
transcription with audio length, so a bare duration is not comparable across
captures.

`total_ms` is measured from timer construction to `finish()`, which means it also
covers the DB writes and salutation detection between stages. Stages will therefore
not sum to the total, and the residue is itself worth seeing.

## Two numbers already available

`captures` already records `created_at`, `reply_ready_at` and `reply_downloaded_at`
at one-second resolution. Their differences give the whole-pipeline duration and,
more usefully, how long the *device* took to notice `reply_ready` and fetch the WAV.

If the 26 seconds is 20 seconds of bridge plus 6 seconds of device poll cadence, no
bridge-side optimization touches that last 6 seconds. The readout reports it so we
do not spend the next cycle on the wrong side of the wire.

## Storage

A new table, so it materializes on next start with no migration against the
deployed database:

```sql
CREATE TABLE IF NOT EXISTS capture_timings (
    capture_id       TEXT PRIMARY KEY,
    kind             TEXT NOT NULL,      -- 'note' | 'conversation' | 'unknown'
    outcome          TEXT NOT NULL,      -- 'ok' or the capture's error slug
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
```

`kind` is `unknown` when the capture failed before its disposition was decided —
a transcription failure has no note-versus-conversation answer.

Explicit stage columns rather than one row per stage: there are four stages, the
readout query stays trivial, and one row per capture keeps the size correlates
beside the durations they explain.

**No retention policy.** `storage.prune()` deletes audio files only; capture rows
already accumulate without bound, and a timing row is smaller than the capture row
it accompanies. Adding a retention rule here would invent a policy the codebase
does not have.

## Modules

### `timing.py`

`CaptureTimer` accumulates one capture's durations in memory. It knows nothing about
SQL or logging.

```python
STAGES = ("transcribe", "agent", "synthesize", "save")

class CaptureTimer:
    def __init__(self, capture_id: str, *,
                 monotonic: Callable[[], float],
                 clock: Callable[[], int]) -> None: ...

    @contextmanager
    def stage(self, name: str) -> Iterator[None]: ...   # records in finally
    def note(self, **sizes: int | None) -> None: ...    # audio_bytes=…, reply_chars=…
    def set_kind(self, kind: str) -> None: ...
    def finish(self, outcome: str) -> CaptureTiming: ...
```

`stage()` rejects a name outside `STAGES`, so a typo fails a test rather than
silently dropping a column. Both time sources are injected, matching the existing
`clock` parameter pattern — a scripted fake monotonic makes durations exactly
assertable rather than approximately.

`CaptureTiming` is a frozen dataclass carrying the row plus `as_log_line()`, so the
log format is testable without standing up a pipeline. A module-level `log_timing()`
emits it on the dedicated `htp_bridge.timing` logger.

`TimingStore` owns the table and nothing else:

```python
class TimingStore:
    def record(self, timing: CaptureTiming) -> None: ...        # INSERT OR REPLACE
    def recent(self, limit: int = 20) -> list[CaptureTiming]: ...
    def summary(self) -> list[StageSummary]: ...        # per (kind, stage), outcome='ok' only
    def failure_counts(self) -> dict[str, int]: ...     # non-'ok' rows, grouped by outcome slug
```

`StageSummary` carries `kind`, `stage`, `count`, `min_ms`, `median_ms`, `p90_ms`,
`max_ms`. Percentiles use nearest-rank on the sorted sample so the test is
deterministic.

`summary()` aggregates only rows with `outcome = 'ok'`. A failed capture's
duration is not a representative sample of that stage's normal cost -- an
`agent_unavailable` timeout dwarfs every real agent call, and a fast failure
(an ingest rejected immediately) reads as an implausibly quick success. At the
handful-of-captures sample sizes this tool is used at, one such row can own
the p90 outright. `failure_counts()` reports what was excluded, as a count per
outcome slug, so the readout can say what it left out instead of silently
dropping it. (Corrected in post-implementation review; see "Review fixes"
below -- the original cut pooled every outcome into one sample.)

`NullTimingStore` implements the same four methods as no-ops.

### `pipeline.py`

`Pipeline.__init__` gains `timings: TimingStore | None = None` and
`monotonic: Callable[[], float] = time.monotonic`, storing
`timings or NullTimingStore()`. Defaulting to a null object rather than `None` keeps
one code path with no `if self._timings` branches, and leaves the 252 existing tests
constructing `Pipeline` unchanged.

`process()` creates the timer and passes it explicitly into `_finish_note` and
`_answer`, wrapping the body in `try/finally` so the row is written on every exit —
including when an exception escapes to the safety net in `api._run_pipeline`.

The timer is created *after* the existing early return for a missing or
already-terminal capture, so a duplicate upload that the pipeline correctly ignores
writes no timing row. Resumed captures do get rows; a resume measures real work.

`outcome` is read from the capture row after the body rather than tracked
separately. A note whose agent ingest failed therefore lands as
`outcome=ingest_failed` for free, because `_finish_note` already writes that
write-ahead flag before calling the agent.

`process()` also catches `BaseException` around the call into `_process()` and
records an `override = "pipeline_error"` when one escapes, then re-raises it
unchanged (corrected in post-implementation review; see below). `_record_timing`
uses `capture.error or override or "ok"` -- the capture row's own error still
wins, so `_finish_note`'s write-ahead `ingest_failed` flag survives an
unrelated exception, but an unexpected failure with no capture-row error no
longer reads back as `outcome=ok`.

The emit step is wrapped so a failing timing write logs and is swallowed. A
diagnostic that can lose someone's note is worse than no diagnostic.

### `captures.py`

One addition: `reply_latencies(limit)` returns the `created_at` /
`reply_ready_at` / `reply_downloaded_at` triples for recent conversations, so the
readout gets its device-lag numbers without reaching across the store boundary into
another module's table.

### `timing_report.py`

A `htp-timings` console script.

```
htp-timings [--db PATH | --config PATH] [--recent N]
```

Default output is the per-stage summary split by kind, followed by the excluded
failure counts (if any) and then the whole-pipeline and device-lag figures.
`--recent N` (N > 0) lists individual rows instead, each with its size
correlates alongside the durations.

`--db` exists alongside `--config` because the config file is mode 600 and the state
directory belongs to the service user; in practice the command runs as that user
against the database path directly. The exact invocation goes in `bridge/README.md`
rather than being something to remember.

Both entry points were tightened in post-implementation review (see "Review
fixes" below): the readout requires `db_path.exists()` before opening it, so a
typo'd `--db` path is reported and exits 1 instead of silently creating an
empty database; and `--recent` only takes the recent-rows branch for a
positive value, since SQLite treats `LIMIT -1` as unbounded.

## Log format

One INFO line per capture on `htp_bridge.timing`, with `timing` as a fixed
greppable token:

```
capture c-0a1f… timing kind=conversation outcome=ok total=25840ms
transcribe=3120ms agent=19980ms synthesize=2610ms save=12ms
audio=131116B transcript=42c reply=88c
```

Emitted as a single line; absent stages are omitted rather than printed as zero.

## Testing

Tests first, per the plan's global constraints. All 252 existing tests stay green.

`tests/test_timing.py`

- Scripted fake monotonic: per-stage durations are exact, not approximate.
- A stage whose body raises still records its duration, and the exception propagates.
- `stage()` rejects an unknown name.
- `as_log_line()` format, including omission of absent stages.
- `TimingStore` round-trip against an in-memory database.
- `summary()` percentile math on a known sample.

`tests/test_pipeline.py` additions

- Conversation path writes a row with all four stages and `kind=conversation`.
- Note path writes `transcribe` and `agent` with `kind=note`.
- A transcription failure writes a partial row with `kind=unknown` and
  `outcome=transcription_failed`.
- A note whose ingest fails records `outcome=ingest_failed`.
- A `TimingStore` that raises on `record()` does not fail the capture.

`tests/test_timing_report.py`

- Rendering of both output modes from seeded rows.

## Review fixes (post-implementation, before merge)

A whole-branch review after all four tasks landed found three correctness gaps
and several readability issues. Fixed, each with a regression test:

- **`summary()` pooled failed and successful captures together.** A timeout
  could own the p90 outright, and a failure that happened to be fast read as
  an implausibly quick success. `summary()` now aggregates `outcome = 'ok'`
  rows only; `TimingStore.failure_counts()` reports what it excluded, and
  `render_summary` prints that beneath the table.
- **An unexpected exception recorded `outcome=ok`.** `_record_timing` ran in
  `process()`'s `finally`, before the caller's own safety net
  (`api._run_pipeline`, `resume()`) had written `failed`/`pipeline_error` to
  the capture row, so it read back a non-terminal row with `error IS NULL` and
  defaulted to `"ok"`. `process()` now catches `BaseException`, records an
  `override = "pipeline_error"`, and re-raises unchanged; `_record_timing`
  uses `capture.error or override or "ok"` so a write-ahead flag (e.g.
  `_finish_note`'s `ingest_failed`) still wins.
- **`render_summary`'s empty-table guard discarded the reply latencies.** A
  freshly redeployed host has an empty `capture_timings` table but a
  `captures` table with real history. The guard now suppresses only the stage
  table (printing "No per-stage timings recorded yet.") and still renders the
  latency block when there's data for it.
- Minor: the summary header's numeric columns are 9 wide to match the
  `{value:>7}ms` data cells (was 8, so the rule under-ran and `max` sat off
  its column); the median column is labelled `p50` rather than `med` (a
  correct nearest-rank `min == p50` on two samples read as a bug under the old
  label); `render_recent`'s `capture_id` column is 24 wide, matching the real
  22-character `c-YYYYMMDD-HHMMSS-XXXX` format; `--recent` rows now include
  size correlates via the new `timing.format_sizes()` helper (shared with
  `as_log_line()`); `--recent` with a value `<= 0` falls back to the summary
  view rather than passing a negative `LIMIT` to SQLite; `htp-timings` checks
  `db_path.exists()` before opening it, so a typo'd `--db` path is reported to
  stderr and exits 1 instead of silently creating an empty database.

## Files touched

New: `timing.py`, `timing_report.py`, `tests/test_timing.py`,
`tests/test_timing_report.py`.
Modified: `db.py` (one table in `SCHEMA`), `pipeline.py` (wiring), `captures.py`
(`reply_latencies`), `main.py` (`build_deps`), `pyproject.toml` (script entry),
`tests/test_pipeline.py`, `bridge/README.md`.

## Deployment

The table materializes on next start; there is no migration step. The redeploy is
the established one, and its known ordering trap still applies: the shared checkout
must be pulled *before* the install script runs, because the script reads the
bridge source from that path — reinstalling first silently redeploys the old code.
The service restart after installation is required; the script's `enable --now` is a
no-op on an already-running unit.
