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
