import pytest

from htp_bridge.timing import CaptureTimer, CaptureTiming, format_sizes


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
    assert store.failure_counts() == {}


def test_summary_excludes_failed_captures_from_aggregation(db):
    store = TimingStore(db)
    # A fast "success" sample of 4800ms would be a plausible ingest time, but
    # here it's a failed ingest -- it must not be read as a fast successful one.
    store.record(
        timing("c-1", kind="note", outcome="ok", stages={"agent": 9000}, total_ms=9100)
    )
    store.record(
        timing(
            "c-2", kind="note", outcome="ingest_failed", stages={"agent": 4800}, total_ms=4900
        )
    )
    # A timeout that would otherwise own the p90 outright at this sample size.
    store.record(
        timing(
            "c-3",
            kind="conversation",
            outcome="agent_unavailable",
            stages={"agent": 28400},
            total_ms=28500,
        )
    )

    summaries = store.summary()

    note_agent = next(s for s in summaries if s.kind == "note" and s.stage == "agent")
    assert note_agent.count == 1
    assert (note_agent.min_ms, note_agent.max_ms) == (9000, 9000)
    assert not any(s.kind == "conversation" for s in summaries), (
        "the only conversation sample is a failure and must not surface at all"
    )


def test_failure_counts_reports_excluded_rows_by_outcome_slug(db):
    store = TimingStore(db)
    store.record(timing("c-1", outcome="ok"))
    store.record(timing("c-2", kind="note", outcome="ingest_failed"))
    store.record(timing("c-3", kind="note", outcome="ingest_failed"))
    store.record(timing("c-4", kind="conversation", outcome="agent_unavailable"))

    assert store.failure_counts() == {"ingest_failed": 2, "agent_unavailable": 1}


def test_format_sizes_only_includes_present_sizes():
    assert format_sizes({}) == ""
    assert format_sizes({"audio_bytes": 8, "reply_chars": 12}) == "audio=8B reply=12c"
