"""여백 FastAPI 앱 — 라우터 등록 + 엔진 로드 + /health (절차서 Phase 3).

실행:  cd yeobaek && uvicorn server.main:app --reload
필요 env: SEOUL_API_KEY(예보), (선택) YEOBAEK_USE_LLM/ANTHROPIC_API_KEY
"""
from __future__ import annotations
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

from .config import settings
from .db import repository
from .engine import engine_state
from .api import match, schedule, card, places, districts

log = logging.getLogger("yeobaek")


@asynccontextmanager
async def lifespan(app: FastAPI):
    # 스키마 멱등 적용(파일이 없거나 실패해도 서버는 뜬다)
    try:
        repository.init_db()
    except Exception as e:  # pragma: no cover
        log.warning("init_db skipped: %s", e)
    # C++ 엔진 로드 + SpatialIndex 적재
    try:
        engine_state.init()
        log.info("engine available=%s places_loaded=%s",
                 engine_state.available, engine_state.places_loaded)
    except Exception as e:  # pragma: no cover
        log.warning("engine init failed: %s", e)
    yield


app = FastAPI(title="여백(Yeobaek) API", version="0.1.0", lifespan=lifespan)
app.include_router(match.router)
app.include_router(schedule.router)
app.include_router(card.router)
app.include_router(places.router)
app.include_router(districts.router)


@app.exception_handler(Exception)
async def unhandled(request: Request, exc: Exception):
    # 예외 응답 규격 통일(부록 3-4)
    log.exception("unhandled error on %s", request.url.path)
    return JSONResponse(status_code=500, content={"error": "internal_error", "detail": str(exc)})


@app.get("/health")
def health() -> dict:
    return {
        "status": "ok",
        "engine_available": engine_state.available,
        "engine_error": engine_state.import_error,
        "places_loaded": engine_state.places_loaded,
        "seoul_api_key_set": bool(settings.SEOUL_API_KEY),
        "llm_enabled": settings.USE_LLM,
    }
