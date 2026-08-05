from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Any, Callable

from fastapi import BackgroundTasks, Depends, FastAPI, Header, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import FileResponse, JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

from htp_bridge.agent import AgentError
from htp_bridge.captures import CaptureStore
from htp_bridge.config import DeviceConfig, ServerConfig
from htp_bridge.dashboard import DashboardStore
from htp_bridge.devices import DeviceRegistry
from htp_bridge.notifications import NotificationStore
from htp_bridge.pipeline import Pipeline
from htp_bridge.storage import AudioStorage, is_valid_capture_id

MAX_STATUS_IDS = 64
EMPTY_REVISION = "0"

# D2: framework-generated errors (unknown route, wrong method) map to slugs by
# status code. Anything else Starlette might raise falls back to a generic slug
# rather than leaking the framework's default {"detail": ...} shape.
_STATUS_SLUGS = {404: "not_found", 405: "method_not_allowed"}
_DEFAULT_HTTP_SLUG = "http_error"


@dataclass
class Deps:
    server: ServerConfig
    captures: CaptureStore
    storage: AudioStorage
    dashboard: DashboardStore
    notifications: NotificationStore
    devices: DeviceRegistry
    pipeline: Pipeline
    agent: Any
    clock: Callable[[], int] = field(default=lambda: int(time.time()))


class HTPError(Exception):
    def __init__(self, status_code: int, slug: str) -> None:
        self.status_code = status_code
        self.slug = slug


def _int_or_none(value: str | None) -> int | None:
    try:
        return int(value) if value is not None else None
    except ValueError:
        return None


def create_app(deps: Deps, lifespan=None) -> FastAPI:
    app = FastAPI(title="HTP Bridge", docs_url=None, redoc_url=None, lifespan=lifespan)

    @app.exception_handler(HTPError)
    async def _htp_error(request: Request, exc: HTPError) -> JSONResponse:
        return JSONResponse(status_code=exc.status_code, content={"error": exc.slug})

    # D2: RequestValidationError (malformed/invalid request bodies FastAPI rejects
    # before our handlers run) and Starlette's HTTPException fallback (unmatched
    # routes/methods) must still produce the {"error": "<slug>"} shape, not the
    # framework's default {"detail": ...}.
    @app.exception_handler(RequestValidationError)
    async def _validation_error(request: Request, exc: RequestValidationError) -> JSONResponse:
        return JSONResponse(status_code=422, content={"error": "invalid_request"})

    @app.exception_handler(StarletteHTTPException)
    async def _http_exception(request: Request, exc: StarletteHTTPException) -> JSONResponse:
        slug = _STATUS_SLUGS.get(exc.status_code, _DEFAULT_HTTP_SLUG)
        return JSONResponse(status_code=exc.status_code, content={"error": slug})

    def authenticate(
        authorization: str | None = Header(default=None),
        x_battery: str | None = Header(default=None),
    ) -> DeviceConfig:
        token = ""
        if authorization and authorization.lower().startswith("bearer "):
            token = authorization[7:].strip()
        device = deps.devices.authenticate(token)
        if device is None:
            raise HTPError(401, "unauthorized")
        deps.devices.record_telemetry(device.id, _int_or_none(x_battery))
        return device

    @app.get("/healthz")
    async def healthz() -> dict[str, Any]:
        return {
            "status": "ok",
            "server_time": deps.clock(),
            "pipeline_backlog": len(deps.captures.unfinished_ids()),
            "ingestion_backlog": len(deps.captures.ingestion_backlog()),
            "devices": [
                {"device_id": s.device_id, "battery": s.battery, "last_seen": s.last_seen}
                for s in deps.devices.statuses()
            ],
        }

    @app.post("/htp/v1/captures")
    async def upload_capture(
        request: Request,
        background: BackgroundTasks,
        device: DeviceConfig = Depends(authenticate),
        x_capture_id: str | None = Header(default=None),
        x_recorded_at: str | None = Header(default=None),
        x_conversation_id: str | None = Header(default=None),
    ) -> dict[str, str]:
        if not x_capture_id:
            raise HTPError(400, "missing_capture_id")
        if not is_valid_capture_id(x_capture_id):
            raise HTPError(400, "invalid_capture_id")

        body = await request.body()
        if not body:
            raise HTPError(400, "empty_capture")
        if len(body) > deps.server.max_upload_bytes:
            raise HTPError(413, "capture_too_large")

        # D1 (human-approved deviation from the brief): write the audio to disk
        # BEFORE creating the capture DB row. The brief's ordering (row first,
        # then WAV) loses audio permanently if the process dies in between --
        # captures.create is idempotent via INSERT OR IGNORE, so a retry after a
        # crash mid-upload becomes a silent no-op with no audio ever recorded.
        # Inverted, a crash after save-but-before-create leaves only a harmless
        # orphan WAV that the retry overwrites.
        deps.storage.save_upload(x_capture_id, body)

        capture, created = deps.captures.create(
            capture_id=x_capture_id,
            device_id=device.id,
            recorded_at=_int_or_none(x_recorded_at),
            conversation_id=x_conversation_id or None,
        )
        if created:
            background.add_task(deps.pipeline.process, x_capture_id)
            return {"id": x_capture_id, "state": "received"}
        return {"id": capture.id, "state": capture.state}

    @app.get("/htp/v1/captures")
    async def capture_status(
        ids: str = "", device: DeviceConfig = Depends(authenticate)
    ) -> dict[str, Any]:
        requested = [part for part in (ids or "").split(",") if part]
        if len(requested) > MAX_STATUS_IDS:
            raise HTPError(400, "too_many_ids")

        found = {capture.id: capture for capture in deps.captures.get_many(requested)}
        entries: list[dict[str, Any]] = []
        for capture_id in requested:
            capture = found.get(capture_id)
            if capture is None:
                entries.append({"id": capture_id, "state": "unknown"})
                continue
            entry: dict[str, Any] = {"id": capture.id, "state": capture.state}
            if capture.transcript is not None:
                entry["transcript"] = capture.transcript
            if capture.conversation_id:
                entry["conversation_id"] = capture.conversation_id
            if capture.error:
                entry["error"] = capture.error
            entries.append(entry)
        return {"server_time": deps.clock(), "captures": entries}

    @app.get("/htp/v1/captures/{capture_id}/reply.wav")
    async def reply_audio(
        capture_id: str, device: DeviceConfig = Depends(authenticate)
    ) -> FileResponse:
        if not is_valid_capture_id(capture_id) or not deps.storage.has_reply(capture_id):
            raise HTPError(404, "reply_not_found")
        deps.captures.mark_downloaded(capture_id)
        return FileResponse(deps.storage.reply_path(capture_id), media_type="audio/wav")

    @app.get("/htp/v1/dashboard")
    async def dashboard(
        rev: str = "", device: DeviceConfig = Depends(authenticate)
    ) -> dict[str, Any]:
        snapshot = deps.dashboard.current()
        now = deps.clock()
        # D3: additive "sync_interval" field, sourced from ServerConfig, on
        # every shape this endpoint returns (empty, unchanged, and full body).
        sync_interval = deps.server.sync_interval_seconds
        if snapshot is None:
            return {
                "rev": EMPTY_REVISION,
                "server_time": now,
                "title": "",
                "items": [],
                "sync_interval": sync_interval,
            }
        if rev and rev == snapshot.rev:
            return {
                "rev": snapshot.rev,
                "unchanged": True,
                "server_time": now,
                "sync_interval": sync_interval,
            }

        items: list[dict[str, Any]] = []
        for item in snapshot.items:
            entry: dict[str, Any] = {"id": item.id, "text": item.text, "done": item.done}
            if item.style:
                entry["style"] = item.style
            items.append(entry)
        return {
            "rev": snapshot.rev,
            "server_time": now,
            "title": snapshot.title,
            "items": items,
            "sync_interval": sync_interval,
        }

    @app.post("/htp/v1/complete")
    async def complete(
        payload: dict[str, Any],
        background: BackgroundTasks,
        device: DeviceConfig = Depends(authenticate),
    ) -> dict[str, Any]:
        item_id = str(payload.get("item_id") or "").strip()
        if not item_id:
            raise HTPError(400, "missing_item_id")

        snapshot = deps.dashboard.current()
        item = next((i for i in snapshot.items if i.id == item_id), None) if snapshot else None
        if item is None:
            raise HTPError(404, "unknown_item")

        rev = deps.dashboard.complete(item_id)
        background.add_task(_notify_completion, deps, item_id, item.text)
        return {"ok": True, "rev": rev}

    @app.get("/htp/v1/notifications")
    async def notifications(device: DeviceConfig = Depends(authenticate)) -> dict[str, Any]:
        return {
            "server_time": deps.clock(),
            "notifications": [
                {
                    "id": n.id,
                    "text": n.text,
                    "priority": n.priority,
                    "created": n.created_at,
                }
                for n in deps.notifications.pending()
            ],
        }

    @app.post("/htp/v1/notifications/ack")
    async def acknowledge(
        payload: dict[str, Any], device: DeviceConfig = Depends(authenticate)
    ) -> dict[str, Any]:
        if "ids" not in payload or not isinstance(payload["ids"], list):
            raise HTPError(400, "missing_ids")
        acked = deps.notifications.ack([str(i) for i in payload["ids"]])
        return {"ok": True, "acked": acked}

    return app


async def _notify_completion(deps: Deps, item_id: str, text: str) -> None:
    """Tell the agent about a completion. Best effort: the snapshot already
    reflects it, and the agent's next publish is authoritative anyway."""
    try:
        await deps.agent.ingest(
            f"The user marked the dashboard item '{text}' (id {item_id}) complete "
            f"on the pocket terminal.",
            None,
        )
    except AgentError:
        pass
