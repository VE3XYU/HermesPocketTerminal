from htp_bridge.captures import ReplyLatency
from htp_bridge.db import Database
from htp_bridge.timing import CaptureTiming, StageSummary, TimingStore
from htp_bridge.timing_report import main, render_recent, render_summary


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


def test_render_summary_still_shows_latencies_when_the_stage_table_is_empty():
    # The realistic state of a freshly redeployed host: capture_timings is a
    # brand-new, empty table, but captures (and therefore reply_latencies) has
    # weeks of history. The old early return threw that data away.
    latencies = [
        ReplyLatency("c-1", pipeline_seconds=20, download_seconds=6),
        ReplyLatency("c-2", pipeline_seconds=24, download_seconds=None),
    ]

    out = render_summary([], latencies)

    assert "no per-stage timings" in out.lower()
    assert "2 conversation(s)" in out
    assert "20" in out and "24" in out


def test_render_summary_reports_excluded_failures_beneath_the_table():
    summaries = [
        StageSummary("conversation", "transcribe", 2, 3120, 3400, 3400, 3400),
        StageSummary("conversation", "agent", 2, 18400, 19980, 19980, 19980),
    ]
    failures = {"ingest_failed": 2, "agent_unavailable": 1}

    out = render_summary(summaries, [], failures)

    exclusion = "3 capture(s) excluded as failed: agent_unavailable 1, ingest_failed 2"
    assert exclusion in out, out
    # The exclusion line sits beneath the table.
    assert out.index("19980ms") < out.index(exclusion)


def test_render_summary_omits_exclusion_line_when_nothing_failed():
    summaries = [StageSummary("conversation", "agent", 2, 100, 200, 300, 300)]

    out = render_summary(summaries, [], {})

    assert "excluded" not in out


def test_summary_header_and_rule_match_data_row_width():
    summaries = [StageSummary("conversation", "agent", 3, 100, 200, 300, 300)]

    out = render_summary(summaries, [])
    header, rule, row = out.splitlines()[:3]

    assert len(header) == len(rule) == len(row)


def test_summary_header_labels_the_median_column_p50():
    out = render_summary([StageSummary("note", "agent", 2, 100, 100, 200, 200)], [])

    header = out.splitlines()[0]
    assert "p50" in header
    assert "med" not in header


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


def test_render_recent_widens_capture_id_column_for_real_ids():
    # Real capture ids are c-YYYYMMDD-HHMMSS-XXXX -- 22 characters.
    capture_id = "c-20260809-153000-a1b2"
    rows = [
        CaptureTiming(capture_id, "note", "ok", 1_000_000, 900, {"transcribe": 400}, {}),
    ]

    out = render_recent(rows)

    assert f"{capture_id:<24} note" in out


def test_render_recent_includes_size_correlates():
    rows = [
        CaptureTiming(
            "c-1", "conversation", "ok", 1_000_000, 25840,
            {"transcribe": 3120, "agent": 19980, "synthesize": 2610, "save": 12},
            {"audio_bytes": 131116, "transcript_chars": 42, "reply_chars": 88, "reply_bytes": 900},
        ),
    ]

    out = render_recent(rows)

    assert "audio=131116B" in out
    assert "transcript=42c" in out
    assert "reply=88c" in out
    assert "reply_wav=900B" in out


def test_render_recent_omits_absent_sizes_for_a_note():
    rows = [
        CaptureTiming(
            "c-2", "note", "ok", 1_000_000, 900,
            {"transcribe": 400, "agent": 500},
            {"audio_bytes": 2048, "transcript_chars": 20},
        ),
    ]

    out = render_recent(rows)

    assert "audio=2048B" in out
    assert "transcript=20c" in out
    assert "reply=" not in out
    assert "reply_wav=" not in out


def test_main_reports_a_missing_config_cleanly_instead_of_raising(capsys):
    exit_code = main(["--config", "/nonexistent/path.toml"])

    assert exit_code == 1
    err = capsys.readouterr().err
    assert "configuration error" in err
    assert "/nonexistent/path.toml" in err


def test_main_negative_recent_falls_back_to_the_summary_view(tmp_path, capsys):
    db_path = tmp_path / "htp.db"
    db = Database(db_path)
    TimingStore(db).record(
        CaptureTiming(
            "c-1", "note", "ok", 1_000_000, 900, {"transcribe": 400, "agent": 500}, {}
        )
    )
    db.close()

    exit_code = main(["--db", str(db_path), "--recent", "-1"])

    out = capsys.readouterr().out
    assert exit_code == 0
    # The summary header, not the per-row `--recent` dump `LIMIT -1` would give.
    assert "kind" in out.splitlines()[0]
    assert "total=900ms" not in out


def test_main_reports_a_missing_db_path_and_does_not_create_one(tmp_path, capsys):
    missing = tmp_path / "does-not-exist.db"

    exit_code = main(["--db", str(missing)])

    err = capsys.readouterr().err
    assert exit_code == 1
    assert str(missing) in err
    assert not missing.exists()
