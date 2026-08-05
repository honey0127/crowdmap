"""여행자 실시간 제보(크라우드소싱, 모듈5) — 'Waze for Tourism' MVP.

POST /api/v1/reports        — 지금 상태 제보(붐빔/한적/팁)
GET  /api/v1/reports        — 반경 내 최근 제보
"""
from __future__ import annotations

from fastapi import APIRouter, HTTPException, Query
from pydantic import BaseModel

from ..db import repository

router = APIRouter(prefix="/api/v1", tags=["reports"])

KINDS = {"busy", "quiet", "tip"}   # 붐벼요 / 한적해요 / 팁


class ReportIn(BaseModel):
    kind: str
    lat: float
    lng: float
    text: str | None = None
    content_id: int | None = None


@router.post("/reports")
def create_report(body: ReportIn) -> dict:
    if body.kind not in KINDS:
        raise HTTPException(status_code=400, detail=f"kind must be one of {sorted(KINDS)}")
    text = (body.text or "").strip()[:280] or None
    rid = repository.add_report(body.kind, body.lat, body.lng, text, body.content_id)
    return {"id": rid, "ok": True}


@router.get("/reports")
def list_reports(lat: float = Query(...), lng: float = Query(...),
                 radius_km: float = Query(3.0, ge=0.2, le=20.0),
                 limit: int = Query(50, ge=1, le=100)) -> dict:
    return {"reports": repository.nearby_reports(lat, lng, radius_km, limit)}
