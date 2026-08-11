"""감성 쌍둥이 탐색 (모듈1) — /match 와 /schedule 이 공유.

흐름(결정 E): 반경 후보(엔진 있으면 C++ SpatialIndex, 없으면 SQL/haversine 폴백)
→ numpy 코사인 top-K → 장소·예보지점 결합. numpy 코사인 단계는 엔진과 무관하므로
C++ 엔진이 없어도(배포 초기 등) /match·/schedule 의 치환 후보 탐색은 계속 동작한다.
"""
from __future__ import annotations
from typing import Optional

from ..db import repository
from ..util import haversine_km
from .embedding import get_store
from ..engine import engine_state

_FALLBACK_CANDIDATE_CAP = 1000  # 엔진 없을 때 SQL 반경 스캔 상한(전수 스캔 자체는 저렴)


def find_twins(source: dict, radius_km: float, top_k: int) -> list[dict]:
    """source 장소의 감성 쌍둥이 top_k. 각 원소:
    {content_id, title, lat, lng, area_name, similarity, dist_km}."""
    if source.get("lat") is None or source.get("lng") is None:
        return []

    if engine_state.available and engine_state.spatial is not None:
        cand_ids = engine_state.query_radius(source["lat"], source["lng"], radius_km)
    else:
        nearby = repository.nearby_places(source["lat"], source["lng"], radius_km,
                                           limit=_FALLBACK_CANDIDATE_CAP)
        cand_ids = [p["content_id"] for p in nearby]
    cand_ids = [c for c in cand_ids if c != source["content_id"]]
    if not cand_ids:
        return []

    ranked = get_store().similar(source["content_id"], cand_ids, top_k)
    if not ranked:
        return []

    twin_ids = [cid for cid, _ in ranked]
    places = repository.get_places(twin_ids)
    area_map = repository.get_area_map(twin_ids)

    out: list[dict] = []
    for cid, sim in ranked:
        p = places.get(cid)
        if not p or p.get("lat") is None:
            continue
        area = area_map.get(cid, (None, None))[0]
        out.append({
            "content_id": cid,
            "title": p["title"],
            "lat": p["lat"],
            "lng": p["lng"],
            "area_name": area,
            "similarity": round(float(sim), 4),
            "dist_km": round(haversine_km(source["lat"], source["lng"], p["lat"], p["lng"]), 2),
        })
    return out


def forecast_level(area_name: Optional[str], arrival_unix: int) -> int:
    """area 예보지점 T시점 혼잡 레벨(1~4). 예보지점 없으면 중립(2).
    엔진이 있으면 C++ 경로, 없으면 py_forecast 폴백(services/py_forecast.py)."""
    if not area_name:
        return 2
    try:
        if engine_state.available:
            return int(engine_state.forecast(area_name, arrival_unix).level)
        from . import py_forecast
        return py_forecast.forecast_level(area_name, arrival_unix)
    except Exception:
        return 2
