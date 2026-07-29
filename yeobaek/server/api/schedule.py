"""POST /api/v1/schedule — 시간의존 스케줄러 (부록 B)."""
from __future__ import annotations

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from ..config import settings
from ..db import repository
from ..engine import engine_state
from ..util import kst_iso_to_unix, unix_to_kst_hhmm, dwell_for_category
from ..services.match_service import find_twins

router = APIRouter(prefix="/api/v1", tags=["schedule"])


class Weights(BaseModel):
    congestion: float = settings.W_CONGESTION
    distance: float = settings.W_DISTANCE
    similarity: float = settings.W_SIMILARITY


class ScheduleRequest(BaseModel):
    start_time: str                      # KST ISO, e.g. "2026-10-11T10:00:00"
    stops: list[int] = Field(min_length=1, max_length=8)
    weights: Weights = Weights()
    allow_substitution: bool = True
    # keep_order=True → 사용자가 고른 순서 그대로 진행(재정렬 X, 도착시점 예보만 채움).
    #   앱의 "내 순서대로" 모드. False 면 혼잡도 예측으로 순서를 자동 재배치.
    keep_order: bool = False


@router.post("/schedule")
def schedule(req: ScheduleRequest) -> dict:
    if not engine_state.available:
        raise HTTPException(status_code=503,
                            detail=f"engine unavailable: {engine_state.import_error}")

    try:
        start_unix = kst_iso_to_unix(req.start_time)
    except ValueError:
        raise HTTPException(status_code=400, detail="start_time must be ISO-8601 (KST)")

    places = repository.get_places(req.stops)
    missing = [s for s in req.stops if s not in places]
    if missing:
        raise HTTPException(status_code=404, detail=f"unknown content_id(s): {missing}")

    area_map = repository.get_area_map(req.stops)

    sched_stops = []
    for sid in req.stops:  # 입력 순서 보존(치환 없는 baseline 비교 기준)
        p = places[sid]
        area = area_map.get(sid, (None, None))[0]
        twins = find_twins(p, settings.DEFAULT_RADIUS_KM, settings.DEFAULT_TOP_K) \
            if req.allow_substitution else []
        # 예보지점이 없는 twin 은 스케줄러가 다룰 수 없으니 제외
        twins = [t for t in twins if t.get("area_name")]
        dwell = dwell_for_category(p.get("cat"), settings.DEFAULT_DWELL_SEC)
        sched_stops.append(engine_state.make_stop(
            content_id=sid, lat=p["lat"], lng=p["lng"], area_name=area,
            dwell_sec=dwell, twins=twins,
        ))

    weights = engine_state.weights(
        req.weights.congestion, req.weights.distance, req.weights.similarity)
    plan = engine_state.optimize(sched_stops, start_unix, weights,
                                 req.allow_substitution, req.keep_order)

    # content_id → title (원본 + 치환 후보 모두 필요)
    all_ids = set(req.stops)
    for ps in plan.ordered:
        all_ids.add(ps.content_id)
    titles = {cid: pl["title"] for cid, pl in repository.get_places(all_ids).items()}

    ordered = []
    for ps in plan.ordered:
        ordered.append({
            "content_id": ps.content_id,
            "title": titles.get(ps.content_id, str(ps.content_id)),
            "arrival": unix_to_kst_hhmm(ps.arrival_unix),
            "forecast_level": ps.forecast_level,
            "substituted_from": ps.substituted_from if ps.substituted_from != -1 else None,
            "lat": ps.lat,
            "lng": ps.lng,
        })

    return {
        "ordered": ordered,
        "total_cost": round(plan.total_cost, 4),
        "saved_congestion_pct": plan.saved_congestion_pct,
    }
