"""GET /api/v1/districts — 지역구(성수·홍대·강남 …) 목록 + 라인업.

플래너 페이지에서 큰 지역구를 고르면 그 중심 좌표 반경의 명소 라인업을 보여준다.
라인업은 nearby_places(반경 검색)를 재사용한다.
"""
from __future__ import annotations

from fastapi import APIRouter, HTTPException, Query

from ..db import repository
from .places import _to_result

router = APIRouter(prefix="/api/v1", tags=["districts"])

# 서울 대표 지역구 — (중심 좌표는 그 동네의 '핵심 지점' 근사)
DISTRICTS = [
    {"key": "seongsu",   "name": "성수",       "lat": 37.5445, "lng": 127.0559, "desc": "카페·편집숍의 감성 거리"},
    {"key": "hongdae",   "name": "홍대",       "lat": 37.5563, "lng": 126.9236, "desc": "젊음과 예술의 번화가"},
    {"key": "gangnam",   "name": "강남",       "lat": 37.4979, "lng": 127.0276, "desc": "쇼핑·트렌드의 중심"},
    {"key": "gyeongbok", "name": "경복궁·북촌", "lat": 37.5796, "lng": 126.9770, "desc": "고궁과 한옥의 정취"},
    {"key": "itaewon",   "name": "이태원",      "lat": 37.5344, "lng": 126.9945, "desc": "이국적 미식과 거리"},
    {"key": "yeouido",   "name": "여의도",      "lat": 37.5219, "lng": 126.9245, "desc": "한강공원과 도심 스카이라인"},
    {"key": "myeongdong","name": "명동",       "lat": 37.5637, "lng": 126.9820, "desc": "쇼핑과 먹거리의 관광 1번지"},
    {"key": "seochon",   "name": "서촌·인사동", "lat": 37.5769, "lng": 126.9700, "desc": "골목과 전통의 예술 동네"},
    {"key": "jamsil",    "name": "잠실",       "lat": 37.5133, "lng": 127.1000, "desc": "호수공원과 대형 명소"},
    {"key": "yongsan",   "name": "용산",       "lat": 37.5299, "lng": 126.9648, "desc": "박물관과 공원, 새 도심"},
]


@router.get("/districts")
def list_districts() -> dict:
    return {"districts": DISTRICTS}


@router.get("/districts/{key}")
def district(key: str,
             radius_km: float = Query(2.5, ge=0.5, le=6.0),
             limit: int = Query(25, ge=1, le=40)) -> dict:
    """지역구 라인업 — 중심 좌표 반경 내 명소를 가까운 순으로."""
    d = next((x for x in DISTRICTS if x["key"] == key), None)
    if d is None:
        raise HTTPException(status_code=404, detail=f"unknown district: {key}")
    rows = repository.nearby_places(d["lat"], d["lng"], radius_km, limit)
    return {"district": d, "places": [_to_result(p) for p in rows]}
