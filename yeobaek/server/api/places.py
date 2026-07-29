"""GET /api/v1/places/search — 이름으로 장소 검색(앱에서 방문지 추가용).
POST /api/v1/places/adhoc — DB 에 없는 임의 위치를 즉석 등록."""
from __future__ import annotations

from fastapi import APIRouter, Query
from pydantic import BaseModel

from ..db import repository
from ..engine import engine_state
from ..services.rag import cat_label

router = APIRouter(prefix="/api/v1", tags=["places"])


def _deepest_cat(cat: str | None) -> str | None:
    if not cat:
        return None
    parts = [c.strip() for c in cat.split(">") if c.strip()]
    return parts[-1] if parts else None


def _to_result(p: dict) -> dict:
    return {
        "content_id": p["content_id"],
        "title": p["title"],
        "addr": p.get("addr"),
        "cat_label": cat_label(_deepest_cat(p.get("cat"))),
        "lat": p.get("lat"),      # 지도 마커용(홈 화면)
        "lng": p.get("lng"),
        "dist_km": p.get("dist_km"),
    }


@router.get("/places/search")
def search(q: str = Query(..., min_length=1, description="장소명 부분일치"),
           limit: int = Query(20, ge=1, le=50)) -> dict:
    rows = repository.search_places(q, limit)
    return {"results": [_to_result(p) for p in rows]}


@router.get("/places/nearby")
def nearby(lat: float = Query(..., description="지도 중심 위도"),
           lng: float = Query(..., description="지도 중심 경도"),
           radius_km: float = Query(3.0, ge=0.2, le=20.0),
           limit: int = Query(12, ge=1, le=30)) -> dict:
    """지도 이동 시 '이 지역 추천' — 중심 좌표 반경 내 명소를 가까운 순으로."""
    rows = repository.nearby_places(lat, lng, radius_km, limit)
    return {"results": [_to_result(p) for p in rows]}


class AdhocPlace(BaseModel):
    title: str
    lat: float
    lng: float


@router.post("/places/adhoc")
def adhoc(body: AdhocPlace) -> dict:
    """DB 에 없는 위치를 즉석 등록 → content_id 발급(어드혹). 최근접 예보지점도 매핑."""
    area = engine_state.nearest_area(body.lat, body.lng)
    area_name = area[0] if area else None
    dist = area[1] if area else None
    title = (body.title or "").strip() or "선택한 위치"
    cid = repository.insert_adhoc_place(title, body.lat, body.lng, area_name, dist)
    return {"content_id": cid, "title": title,
            "lat": body.lat, "lng": body.lng, "area_name": area_name}
