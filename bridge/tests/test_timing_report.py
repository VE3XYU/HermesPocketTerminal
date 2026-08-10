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
