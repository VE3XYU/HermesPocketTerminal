from __future__ import annotations

import argparse
import sys
from pathlib import Path

from htp_bridge.captures import CaptureStore, ReplyLatency
from htp_bridge.config import ConfigError, load_config
from htp_bridge.db import Database
from htp_bridge.timing import STAGES, CaptureTiming, StageSummary, TimingStore, format_sizes

# Numeric columns are 9 wide to match the data rows below, where each cell is a
# 7-wide right-justified number followed by the fixed 2-char "ms" suffix
# (`{value:>7}ms`). Keep these in lockstep -- a rule of `"-" * len(_HEADER)`
# only lines up under the data if the header is exactly as wide as they are.
_HEADER = f"{'kind':<13} {'stage':<11} {'n':>4} {'min':>9} {'p50':>9} {'p90':>9} {'max':>9}"


def render_summary(
    summaries: list[StageSummary],
    latencies: list[ReplyLatency],
    failures: dict[str, int] | None = None,
) -> str:
    failures = failures or {}
    if not summaries and not latencies and not failures:
        return "No timings recorded yet."

    if summaries:
        lines = [_HEADER, "-" * len(_HEADER)]
        for row in summaries:
            lines.append(
                f"{row.kind:<13} {row.stage:<11} {row.count:>4} "
                f"{row.min_ms:>7}ms {row.median_ms:>7}ms {row.p90_ms:>7}ms {row.max_ms:>7}ms"
            )
    else:
        # A brand-new capture_timings table (e.g. right after a redeploy) is
        # legitimately empty while the captures table -- and therefore the reply
        # latencies below -- already has weeks of history. Only the stage table
        # is missing; say so, rather than claiming there is nothing to report.
        lines = ["No per-stage timings recorded yet."]

    if failures:
        excluded = sum(failures.values())
        detail = ", ".join(f"{slug} {count}" for slug, count in sorted(failures.items()))
        lines.append("")
        lines.append(f"{excluded} capture(s) excluded as failed: {detail}")

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
        # 24-wide: real capture ids are c-YYYYMMDD-HHMMSS-XXXX, 22 characters.
        line = (
            f"{row.started_at} {row.capture_id:<24} {row.kind:<12} "
            f"{row.outcome:<22} total={row.total_ms}ms"
        )
        # Size correlates (only the ones present -- a note has no reply sizes):
        # a duration alone is not comparable across captures, since synthesis
        # scales with reply length and transcription with audio length.
        for extra in (stages, format_sizes(row.sizes)):
            if extra:
                line += f" {extra}"
        lines.append(line)
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

    if args.db:
        db_path = Path(args.db)
    else:
        try:
            db_path = load_config(Path(args.config)).storage.db_path
        except ConfigError as exc:
            print(f"configuration error: {exc}", file=sys.stderr)
            return 1

    # Database(path) creates the file and schema if it doesn't already exist, so
    # a typo'd path would otherwise print "No timings recorded yet." and exit 0
    # -- indistinguishable from a real, empty database. The readout only ever
    # reads a database the bridge service already created.
    if not db_path.exists():
        print(f"no such database: {db_path}", file=sys.stderr)
        return 1

    db = Database(db_path)
    try:
        timings = TimingStore(db)
        if args.recent and args.recent > 0:
            print(render_recent(timings.recent(limit=args.recent)))
        else:
            print(
                render_summary(
                    timings.summary(),
                    CaptureStore(db).reply_latencies(),
                    timings.failure_counts(),
                )
            )
    finally:
        db.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
