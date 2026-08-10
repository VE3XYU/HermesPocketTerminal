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
