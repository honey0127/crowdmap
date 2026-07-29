"""GET /api/v1/places/search — 이름으로 장소 검색(앱에서 방문지 추가용).
POST /api/v1/places/adhoc — DB 에 없는 임의 위치를 즉석 등록.
GET  /api/v1/places/heatmap — 주변 명소의 현재 혼잡 레벨 + 한적함 지수(히트맵용)."""
from __future__ import annotations
import time

from fastapi import APIRouter, Query
from pydantic import BaseModel

from ..db import repository
from ..engine import engine_state
from ..services.rag import cat_label


def quiet_score(level) -> int | None:
    """한적함 지수(0~100). 혼잡 레벨 1(여유)=100 … 4(붐빔)=0. level 없으면 None."""
    if not level:
        return None
    return round((4 - int(level)) / 3 * 100)

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


@router.get("/places/heatmap")
def heatmap(lat: float = Query(...), lng: float = Query(...),
            radius_km: float = Query(3.0, ge=0.2, le=20.0),
            limit: int = Query(24, ge=1, le=40)) -> dict:
    """주변 명소 + 각 명소의 현재 혼잡 레벨(1~4)·한적함 지수. 지도 히트맵/핀 색용.
    서울 예보권 밖(예보지점 미매핑)은 level=null → 앱이 중립 색으로 표시."""
    rows = repository.nearby_places(lat, lng, radius_km, limit)
    now = int(time.time())
    amap = repository.get_area_map([r["content_id"] for r in rows])
    out = []
    for r in rows:
        area = amap.get(r["content_id"], (None, None))[0]
        level = None
        if area and engine_state.available:
            try:
                fc = engine_state.forecast(area, now)
                level = int(fc.level) if fc else None
            except Exception:
                level = None
        item = _to_result(r)
        item["level"] = level
        item["quiet_score"] = quiet_score(level)
        out.append(item)
    return {"results": out}


def _with_congestion(rows: list[dict], now: int) -> list[dict]:
    """장소 목록에 현재 혼잡 레벨 + 한적함 지수를 붙인다."""
    amap = repository.get_area_map([r["content_id"] for r in rows])
    out = []
    for r in rows:
        area = amap.get(r["content_id"], (None, None))[0]
        level = None
        if area and engine_state.available:
            try:
                fc = engine_state.forecast(area, now)
                level = int(fc.level) if fc else None
            except Exception:
                level = None
        item = _to_result(r)
        item["level"] = level
        item["quiet_score"] = quiet_score(level)
        out.append(item)
    return out


@router.get("/places/disperse/{content_id}")
def disperse(content_id: int,
             radius_km: float = Query(0.9, ge=0.2, le=3.0),
             limit: int = Query(8, ge=1, le=20)) -> dict:
    """미시적 분산 — 이 명소 주변(도보권 ~0.9km)의 **더 한적한** 대안을 추천.
    혼잡 레벨 낮은 순으로 정렬(엔진/키 없으면 거리순)."""
    p = repository.get_place(content_id)
    if not p:
        raise HTTPException(status_code=404, detail=f"unknown content_id: {content_id}")
    rows = [r for r in repository.nearby_places(p["lat"], p["lng"], radius_km, 40)
            if r["content_id"] != content_id]
    items = _with_congestion(rows, int(time.time()))
    items.sort(key=lambda x: (x["level"] if x["level"] else 9, x.get("dist_km") or 9.9))
    return {"source_id": content_id, "results": items[:limit]}


@router.get("/resolve_now")
def resolve_now(lat: float = Query(...), lng: float = Query(...)) -> dict:
    """실시간 현재 혼잡(모듈4) — 다음 장소 급증 감지·리스케줄 알림용. 서울권만 valid."""
    r = engine_state.resolve_now(lat, lng)
    if r is None:
        return {"level": None, "valid": False, "quiet_score": None}
    level, ratio, valid = r
    return {"level": level, "ratio": ratio, "valid": valid,
            "quiet_score": quiet_score(level)}


@router.get("/places/offpeak/{content_id}")
def offpeak(content_id: int) -> dict:
    """오프피크 시간 추천 — 향후 12시간 중 혼잡이 낮은 시간대(도착시점 예보 기준).
    서울 예보권 밖이면 timeline 빈 배열."""
    area = repository.get_area_name(content_id)
    timeline = []
    if area and engine_state.available:
        now = int(time.time())
        base = now - (now % 3600) + 3600     # 다음 정시부터
        for h in range(12):
            t = base + h * 3600
            try:
                fc = engine_state.forecast(area, t)
                lvl = int(fc.level) if fc else None
            except Exception:
                lvl = None
            if lvl:
                timeline.append({"unix": t, "level": lvl, "quiet_score": quiet_score(lvl)})
    best = sorted(timeline, key=lambda x: (x["level"], x["unix"]))[:3]
    return {"content_id": content_id, "area": area, "best": best, "timeline": timeline}


class AdhocPlace(BaseModel):
    title: str
    lat: float
    lng: float


@router.post("/places/adhoc")
def adhoc(body: AdhocPlace) -> dict:
    """DB 에 없는 위치를 즉석 등록 → content_id 발급(어드혹). 서울 예보권이면 예보지점도 매핑."""
    area = engine_state.nearest_area(body.lat, body.lng)
    # 서울 예보권(≤4km) 밖이면 매핑 안 함 → 예보는 중립 폴백(전국 대응)
    if area and area[1] <= 4.0:
        area_name, dist = area[0], area[1]
    else:
        area_name, dist = None, None
    title = (body.title or "").strip() or "선택한 위치"
    cid = repository.insert_adhoc_place(title, body.lat, body.lng, area_name, dist)
    return {"content_id": cid, "title": title,
            "lat": body.lat, "lng": body.lng, "area_name": area_name}
