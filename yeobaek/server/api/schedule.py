"""POST /api/v1/schedule — 시간의존 스케줄러 (부록 B)."""
from __future__ import annotations

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from ..config import settings
from ..db import repository
from ..engine import engine_state
from ..util import kst_iso_to_unix, unix_to_kst_hhmm, dwell_for_category, haversine_km
from ..services.match_service import find_twins
from ..services import py_scheduler

router = APIRouter(prefix="/api/v1", tags=["schedule"])

# 여백 지수(0~100) 합산 가중치 — 혼잡·유사도·이동효율을 한 숫자로(스토어 스크린샷·브랜드 언어용).
# 첫 튜닝값(부록 C 스타일 근사): 혼잡 회피가 가장 크게, 이동 효율은 가장 작게.
YEOBAEK_INDEX_W_CONGESTION = 0.45
YEOBAEK_INDEX_W_SIMILARITY = 0.30
YEOBAEK_INDEX_W_TRAVEL = 0.25
_TRAVEL_SCORE_CEIL_KM = 12.0  # 평균 구간 이동거리가 이 값 이상이면 이동효율 점수 0


def _yeobaek_index(ordered: list) -> int:
    """혼잡도(최종 동선의 한적함)·유사도(치환 시 감성 보존)·이동효율(구간 거리)를
    하나의 0~100 점수로 합산. ordered 는 engine_state.optimize() 의 plan.ordered(PlanStop 리스트)."""
    if not ordered:
        return 0

    congestion_score = sum((4 - s.forecast_level) / 3 * 100 for s in ordered) / len(ordered)
    similarity_score = sum(s.similarity for s in ordered) / len(ordered) * 100

    if len(ordered) > 1:
        legs_km = [
            haversine_km(a.lat, a.lng, b.lat, b.lng)
            for a, b in zip(ordered, ordered[1:])
            if a.lat and a.lng and b.lat and b.lng
        ]
        avg_leg_km = sum(legs_km) / len(legs_km) if legs_km else 0.0
    else:
        avg_leg_km = 0.0
    travel_score = max(0.0, 100.0 * (1 - min(1.0, avg_leg_km / _TRAVEL_SCORE_CEIL_KM)))

    score = (YEOBAEK_INDEX_W_CONGESTION * congestion_score
             + YEOBAEK_INDEX_W_SIMILARITY * similarity_score
             + YEOBAEK_INDEX_W_TRAVEL * travel_score)
    return round(max(0.0, min(100.0, score)))


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
    # 엔진(.so) 이 없어도(배포 초기·아키텍처 불일치 등) 여기서 503 으로 끝내지 않는다 —
    # py_scheduler(순수 파이썬 폴백)로 계속 서비스한다. 아래에서 스케줄러 종류에 따라
    # 분기하되, 이후 코드(제목 매핑·yeobaek_index 등)는 두 경로가 동일한 필드 모양
    # (content_id/substituted_from/area_name/arrival_unix/forecast_level/lat/lng/similarity)
    # 을 갖는 결과를 돌려주므로 공통으로 처리한다.
    try:
        start_unix = kst_iso_to_unix(req.start_time)
    except ValueError:
        raise HTTPException(status_code=400, detail="start_time must be ISO-8601 (KST)")

    places = repository.get_places(req.stops)
    missing = [s for s in req.stops if s not in places]
    if missing:
        raise HTTPException(status_code=404, detail=f"unknown content_id(s): {missing}")

    area_map = repository.get_area_map(req.stops)
    use_engine = engine_state.available

    sched_stops = []
    for sid in req.stops:  # 입력 순서 보존(치환 없는 baseline 비교 기준)
        p = places[sid]
        area = area_map.get(sid, (None, None))[0]
        twins = find_twins(p, settings.DEFAULT_RADIUS_KM, settings.DEFAULT_TOP_K) \
            if req.allow_substitution else []
        # 예보지점이 없는 twin 은 스케줄러가 다룰 수 없으니 제외
        twins = [t for t in twins if t.get("area_name")]
        dwell = dwell_for_category(p.get("cat"), settings.DEFAULT_DWELL_SEC)
        if use_engine:
            sched_stops.append(engine_state.make_stop(
                content_id=sid, lat=p["lat"], lng=p["lng"], area_name=area,
                dwell_sec=dwell, twins=twins,
            ))
        else:
            sched_stops.append({
                "content_id": sid, "lat": p["lat"], "lng": p["lng"], "area_name": area,
                "dwell_sec": dwell, "twins": twins,
            })

    weights_dict = {
        "congestion": req.weights.congestion,
        "distance": req.weights.distance,
        "similarity": req.weights.similarity,
    }
    if use_engine:
        weights = engine_state.weights(
            req.weights.congestion, req.weights.distance, req.weights.similarity)
        plan = engine_state.optimize(sched_stops, start_unix, weights,
                                     req.allow_substitution, req.keep_order)
    else:
        plan = py_scheduler.optimize(sched_stops, start_unix, weights_dict,
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
        "yeobaek_index": _yeobaek_index(plan.ordered),
        "scheduler_mode": "cpp" if use_engine else "python-fallback",
    }
