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
from .related import rank_boost, related_names
from ..engine import engine_state

_FALLBACK_CANDIDATE_CAP = 1000  # 엔진 없을 때 SQL 반경 스캔 상한(전수 스캔 자체는 저렴)

# 결합 가중치 — 임베딩(우리 계산)이 주, 공사 연관 관광지가 보정.
# 연관 목록에 있으면 확실히 앞으로 당겨지되, 감성이 전혀 다른 곳이 1위가 되진 않게 한다.
W_EMBEDDING = 1.0
W_RELATED = 0.6

# 임베딩 후보를 top_k 보다 넉넉히 뽑아둬야 연관도 보정이 순위를 바꿀 여지가 생긴다.
_POOL_FACTOR = 4
# 임베딩 벡터가 아예 없을 때(embeddings.npy 미구축) 연관도만으로 고를 후보 상한.
_RELATED_ONLY_CAP = 300


def find_twins(source: dict, radius_km: float, top_k: int) -> list[dict]:
    """source 장소의 감성 쌍둥이 top_k.

    후보 점수 = e5 임베딩 유사도(자체 계산) ⊕ 한국관광공사 연관 관광지(공식 데이터).
    연관 관광지는 '이 곳을 찾은 사람들이 함께 찾은 곳'이라 대안 추천의 근거가 된다.
    키 미설정·쿼터 초과·장애 시에는 임베딩만으로, 임베딩이 없으면 연관도만으로 동작한다.

    각 원소: {content_id, title, lat, lng, area_name, similarity, dist_km,
              related_rank, basis}
      - similarity: 임베딩 코사인(없으면 연관도 대체값)
      - related_rank: 공사 연관 목록에서의 순위(없으면 None)
      - basis: "both" | "embedding" | "related" — 무엇에 근거한 추천인지
    """
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

    # 공사 연관 관광지(장소명 기반). 실패해도 빈 리스트 — 서비스는 계속 동작.
    related = related_names(source.get("title") or "")

    # 임베딩 후보를 넉넉히 뽑아 연관도로 재정렬한다.
    pool = get_store().similar(source["content_id"], cand_ids, max(top_k * _POOL_FACTOR, top_k))
    emb = {cid: float(sim) for cid, sim in pool}

    # 임베딩이 없으면 연관도만으로라도 고른다(벡터 미구축 상태에서도 기능이 살아있게).
    scan_ids = list(emb.keys()) if emb else cand_ids[:_RELATED_ONLY_CAP]
    if not scan_ids:
        return []

    places = repository.get_places(scan_ids)

    scored: list[tuple[float, int, float, float]] = []   # (score, cid, sim, boost)
    for cid in scan_ids:
        p = places.get(cid)
        if not p or p.get("lat") is None:
            continue
        sim = emb.get(cid, 0.0)
        boost = rank_boost(related, p.get("title") or "")
        score = W_EMBEDDING * sim + W_RELATED * boost
        if score <= 0:
            continue          # 임베딩도 연관도도 없는 후보는 근거가 없다
        scored.append((score, cid, sim, boost))

    if not scored:
        return []
    scored.sort(key=lambda t: -t[0])

    area_map = repository.get_area_map([cid for _, cid, _, _ in scored[:top_k]])

    out: list[dict] = []
    for score, cid, sim, boost in scored[:top_k]:
        p = places[cid]
        area = area_map.get(cid, (None, None))[0]
        rrank = _related_rank_of(related, p.get("title") or "")
        out.append({
            "content_id": cid,
            "title": p["title"],
            "lat": p["lat"],
            "lng": p["lng"],
            "area_name": area,
            # 임베딩이 없을 땐 연관도를 유사도 자리에 넣되, basis 로 근거를 밝힌다.
            "similarity": round(sim if sim > 0 else boost, 4),
            "dist_km": round(haversine_km(source["lat"], source["lng"], p["lat"], p["lng"]), 2),
            "related_rank": rrank,
            "basis": ("both" if sim > 0 and boost > 0 else
                      "embedding" if sim > 0 else "related"),
        })
    return out


def _related_rank_of(related: list[dict], title: str) -> int | None:
    """후보가 공사 연관 목록 몇 위인지(없으면 None) — 근거 표시용."""
    if not related or not title:
        return None
    from .related import _norm
    key = _norm(title)
    for i, r in enumerate(related):
        rn = _norm(r.get("name") or "")
        if rn and (rn == key or rn in key or key in rn):
            return r.get("rank") or (i + 1)
    return None


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
