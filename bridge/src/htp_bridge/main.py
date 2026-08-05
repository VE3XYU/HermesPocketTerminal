from __future__ import annotations

import argparse
import asyncio
import contextlib
import logging
import sys
from pathlib import Path

import uvicorn
from fastapi import FastAPI

from htp_bridge.agent import AgentClient
from htp_bridge.api import Deps, create_app
from htp_bridge.captures import CaptureStore
from htp_bridge.config import Config, ConfigError, load_config
from htp_bridge.dashboard import DashboardStore
from htp_bridge.db import Database
from htp_bridge.devices import DeviceRegistry
from htp_bridge.mcp_server import HermesTools, create_mcp_server
from htp_bridge.mock import create_mock_app
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.speech import build_speech_provider
from htp_bridge.storage import AudioStorage

SWEEP_INTERVAL_SECONDS = 60
PRUNE_INTERVAL_SECONDS = 3600

log = logging.getLogger("htp_bridge")


def build_deps(config: Config) -> tuple[Deps, HermesTools]:
    db = Database(config.storage.db_path)
    captures = CaptureStore(db)
    storage = AudioStorage(config.storage)
    notifications = NotificationStore(db)
    dashboard = DashboardStore(db, config.dashboard)
    devices = DeviceRegistry(db, config.devices)
    agent = AgentClient(config.agent)
    pipeline = Pipeline(
        captures=captures,
        storage=storage,
        notifications=notifications,
        speech=build_speech_provider(config.speech),
        agent=agent,
        salutation_prefixes=config.salutations.prefixes,
    )
    deps = Deps(
        server=config.server,
        captures=captures,
        storage=storage,
        dashboard=dashboard,
        notifications=notifications,
        devices=devices,
        pipeline=pipeline,
        agent=agent,
    )
    tools = HermesTools(dashboard=dashboard, notifications=notifications, devices=devices)
    return deps, tools


async def _background_loop(deps: Deps) -> None:
    """Retry unfinished work and move unclaimed replies into the queue."""
    elapsed = 0
    while True:
        await asyncio.sleep(SWEEP_INTERVAL_SECONDS)
        elapsed += SWEEP_INTERVAL_SECONDS
        try:
            await deps.pipeline.sweep_redirects()
            await deps.pipeline.sweep_ingestion()
            if elapsed >= PRUNE_INTERVAL_SECONDS:
                elapsed = 0
                deps.storage.prune()
        except Exception:
            log.exception("background sweep failed")


def create_full_app(config: Config) -> FastAPI:
    deps, tools = build_deps(config)
    mcp_app = create_mcp_server(tools).streamable_http_app()

    @contextlib.asynccontextmanager
    async def lifespan(app: FastAPI):
        # Starlette does not run a mounted sub-app's lifespan, so the MCP session
        # manager is started explicitly here. Without this, /mcp returns errors.
        async with mcp_app.router.lifespan_context(mcp_app):
            # D4: resume() must complete before the app starts serving requests --
            # see Pipeline.resume()'s docstring on why concurrent invocation with
            # live process() calls can double-deliver. It is awaited here, ahead
            # of the yield, so nothing below can begin serving until it finishes.
            resumed = await deps.pipeline.resume()
            if resumed:
                log.info("resumed %d unfinished capture(s)", resumed)
            sweeper = asyncio.create_task(_background_loop(deps))
            try:
                yield
            finally:
                sweeper.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await sweeper

    app = create_app(deps, lifespan=lifespan)
    app.mount("/mcp", mcp_app)
    return app


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="htp-bridge")
    parser.add_argument("--config", default="/etc/htp-bridge/config.toml")
    parser.add_argument("--mock", action="store_true", help="serve canned responses")
    parser.add_argument("--host", default=None)
    parser.add_argument("--port", type=int, default=None)
    args = parser.parse_args(argv)

    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s %(message)s"
    )

    if args.mock:
        uvicorn.run(create_mock_app(), host=args.host or "127.0.0.1", port=args.port or 8787)
        return 0

    try:
        config = load_config(Path(args.config))
    except ConfigError as exc:
        print(f"configuration error: {exc}", file=sys.stderr)
        return 1

    uvicorn.run(
        create_full_app(config),
        host=args.host or config.server.host,
        port=args.port or config.server.port,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
