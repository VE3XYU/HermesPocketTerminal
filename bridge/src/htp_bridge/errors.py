from __future__ import annotations

import logging

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from starlette.exceptions import HTTPException as StarletteHTTPException

log = logging.getLogger(__name__)

# D2: framework-generated errors (unknown route, wrong method) map to slugs by
# status code. Anything else Starlette might raise falls back to a generic slug
# rather than leaking the framework's default {"detail": ...} shape.
_STATUS_SLUGS = {404: "not_found", 405: "method_not_allowed"}
_DEFAULT_HTTP_SLUG = "http_error"


class HTPError(Exception):
    def __init__(self, status_code: int, slug: str) -> None:
        self.status_code = status_code
        self.slug = slug


def install_error_handlers(app: FastAPI) -> None:
    """Give every error path the {"error": "<slug>"} envelope.

    Shared by the real API and the mock so firmware developed against
    `bridge --mock` sees exactly the shapes the real bridge produces.
    """

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

    # D2 (completed per review): any exception not already caught by a more
    # specific handler above must still produce the {"error": "<slug>"} shape
    # rather than Starlette's default text/plain 500 traceback page.
    @app.exception_handler(Exception)
    async def _unhandled_exception(request: Request, exc: Exception) -> JSONResponse:
        log.exception("unhandled exception on %s %s", request.method, request.url.path)
        return JSONResponse(status_code=500, content={"error": "internal_error"})
