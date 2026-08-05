from __future__ import annotations

import time
from typing import Any, Callable

from fastapi import FastAPI, Request
from fastapi.responses import Response

from htp_bridge.errors import install_error_handlers

MOCK_WAV = b"RIFF" + b"\x00" * 128
MOCK_REV = "mockrev1"
MOCK_SYNC_INTERVAL = 600  # ServerConfig's sync_interval_seconds default
MOCK_ITEMS = [
    {"id": "t-9f2", "text": "Buy milk", "done": False},
    {"id": "t-c41", "text": "Call dentist", "done": True, "style": "dim"},
]


def create_mock_app(clock: Callable[[], int] = lambda: int(time.time())) -> FastAPI:
    """Canned HTP responses for firmware development.

    Accepts any token and touches no database, speech provider, or agent, so a
    device can be brought up against a laptop with nothing else running.
    """
    app = FastAPI(title="HTP Bridge (mock)", docs_url=None, redoc_url=None)
    # D2 parity with the real API: firmware developed against the mock must see
    # the same {"error": "<slug>"} envelope, never FastAPI's {"detail": ...}.
    install_error_handlers(app)

    @app.get("/healthz")
    async def healthz() -> dict[str, Any]:
        return {"status": "ok", "mock": True, "server_time": clock()}

    @app.post("/htp/v1/captures")
    async def upload(request: Request) -> dict[str, str]:
        await request.body()
        return {"id": request.headers.get("x-capture-id", "c-mock"), "state": "received"}

    @app.get("/htp/v1/captures")
    async def status(ids: str = "") -> dict[str, Any]:
        requested = [part for part in ids.split(",") if part] or ["c-mock"]
        return {
            "server_time": clock(),
            "captures": [
                {
                    "id": capture_id,
                    "state": "reply_ready",
                    "transcript": "Hey Hermes, what's on my calendar today?",
                    "conversation_id": "v-mock",
                }
                for capture_id in requested
            ],
        }

    @app.get("/htp/v1/captures/{capture_id}/reply.wav")
    async def reply(capture_id: str) -> Response:
        return Response(content=MOCK_WAV, media_type="audio/wav")

    @app.get("/htp/v1/dashboard")
    async def dashboard(rev: str = "") -> dict[str, Any]:
        if rev == MOCK_REV:
            return {
                "rev": MOCK_REV,
                "unchanged": True,
                "server_time": clock(),
                "sync_interval": MOCK_SYNC_INTERVAL,
            }
        return {
            "rev": MOCK_REV,
            "server_time": clock(),
            "title": "Today",
            "items": MOCK_ITEMS,
            "sync_interval": MOCK_SYNC_INTERVAL,
        }

    @app.post("/htp/v1/complete")
    async def complete(payload: dict[str, Any]) -> dict[str, Any]:
        return {"ok": True, "rev": "mockrev2"}

    @app.get("/htp/v1/notifications")
    async def notifications() -> dict[str, Any]:
        return {
            "server_time": clock(),
            "notifications": [
                {
                    "id": "n-1",
                    "text": "Meeting with Alex at 10:00 AM",
                    "priority": "urgent",
                    "created": clock(),
                }
            ],
        }

    @app.post("/htp/v1/notifications/ack")
    async def acknowledge(payload: dict[str, Any]) -> dict[str, Any]:
        return {"ok": True, "acked": len(payload.get("ids", []))}

    return app
