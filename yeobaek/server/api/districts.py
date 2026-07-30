"""GET /api/v1/districts — 지역구(성수·홍대·강남 …) 목록 + 라인업.

플래너 페이지에서 큰 지역구를 고르면 그 중심 좌표 반경의 명소 라인업을 보여준다.
라인업은 nearby_places(반경 검색)를 재사용한다.
"""
from __future__ import annotations

import datetime

from fastapi import APIRouter, HTTPException, Query

from ..db import repository
from ..services.tats import TatsClient, TatsError
from .places import _to_result, quiet_score

router = APIRouter(prefix="/api/v1", tags=["districts"])

# 전국 대표 지역 — 중심 좌표 + 전국 집중률 지역코드(area_cd 시도, signgu_cd 시군구).
# signgu_cd 는 표준 법정동 시군구 코드(집중률 API 매칭용). 서울 구 코드는 안정적,
# 그 외는 일부 검증 필요할 수 있고 불일치 시 혼잡도는 조용히 비워진다.
DISTRICTS = [
    # 서울/수도권
    {"key": "seongsu",   "name": "성수",       "region": "서울", "lat": 37.5445, "lng": 127.0559, "desc": "카페·편집숍의 감성 거리", "area_cd": "11", "signgu_cd": "11200"},
    {"key": "hongdae",   "name": "홍대",       "region": "서울", "lat": 37.5563, "lng": 126.9236, "desc": "젊음과 예술의 번화가",   "area_cd": "11", "signgu_cd": "11440"},
    {"key": "gangnam",   "name": "강남",       "region": "서울", "lat": 37.4979, "lng": 127.0276, "desc": "쇼핑·트렌드의 중심",     "area_cd": "11", "signgu_cd": "11680"},
    {"key": "gyeongbok", "name": "경복궁·북촌", "region": "서울", "lat": 37.5796, "lng": 126.9770, "desc": "고궁과 한옥의 정취",     "area_cd": "11", "signgu_cd": "11110"},
    {"key": "itaewon",   "name": "이태원",      "region": "서울", "lat": 37.5344, "lng": 126.9945, "desc": "이국적 미식과 거리",     "area_cd": "11", "signgu_cd": "11170"},
    {"key": "myeongdong","name": "명동",       "region": "서울", "lat": 37.5637, "lng": 126.9820, "desc": "쇼핑과 먹거리의 관광 1번지","area_cd": "11", "signgu_cd": "11140"},
    {"key": "seochon",   "name": "서촌·인사동", "region": "서울", "lat": 37.5769, "lng": 126.9700, "desc": "골목과 전통의 예술 동네", "area_cd": "11", "signgu_cd": "11110"},
    {"key": "jamsil",    "name": "잠실",       "region": "서울", "lat": 37.5133, "lng": 127.1000, "desc": "호수공원과 대형 명소",   "area_cd": "11", "signgu_cd": "11710"},
    {"key": "incheon",   "name": "인천 개항장", "region": "수도권", "lat": 37.4759, "lng": 126.6169, "desc": "개항의 흔적과 근대 거리", "area_cd": "28", "signgu_cd": "28110"},
    {"key": "suwon",     "name": "수원 화성",  "region": "수도권", "lat": 37.2881, "lng": 127.0140, "desc": "세계유산 성곽 도시",     "area_cd": "41", "signgu_cd": "41115"},
    # 강원
    {"key": "gangneung", "name": "강릉",       "region": "강원", "lat": 37.7519, "lng": 128.8761, "desc": "바다와 커피의 도시",     "area_cd": "51", "signgu_cd": "51150"},
    {"key": "sokcho",    "name": "속초",       "region": "강원", "lat": 38.2070, "lng": 128.5918, "desc": "설악과 동해 사이",       "area_cd": "51", "signgu_cd": "51210"},
    {"key": "chuncheon", "name": "춘천",       "region": "강원", "lat": 37.8813, "lng": 127.7300, "desc": "호수와 낭만의 도시",     "area_cd": "51", "signgu_cd": "51110"},
    # 충청
    {"key": "daejeon",   "name": "대전",       "region": "충청", "lat": 36.3504, "lng": 127.3845, "desc": "과학과 온천의 도시",     "area_cd": "30", "signgu_cd": "30200"},
    # 영남
    {"key": "haeundae",  "name": "부산 해운대", "region": "영남", "lat": 35.1587, "lng": 129.1604, "desc": "해변과 마천루의 바다 도시","area_cd": "26", "signgu_cd": "26350"},
    {"key": "nampo",     "name": "부산 남포·광복", "region": "영남", "lat": 35.0975, "lng": 129.0306, "desc": "항구와 시장, 원도심",  "area_cd": "26", "signgu_cd": "26110"},
    {"key": "gyeongju",  "name": "경주",       "region": "영남", "lat": 35.8356, "lng": 129.2194, "desc": "천년 신라의 야외 박물관", "area_cd": "47", "signgu_cd": "47130"},
    {"key": "daegu",     "name": "대구",       "region": "영남", "lat": 35.8668, "lng": 128.5940, "desc": "골목과 시장의 도시",     "area_cd": "27", "signgu_cd": "27110"},
    {"key": "tongyeong", "name": "통영",       "region": "영남", "lat": 34.8544, "lng": 128.4331, "desc": "한려수도의 미항",       "area_cd": "48", "signgu_cd": "48220"},
    {"key": "andong",    "name": "안동 하회",  "region": "영남", "lat": 36.5390, "lng": 128.5176, "desc": "종가와 전통의 고장",     "area_cd": "47", "signgu_cd": "47170"},
    # 호남
    {"key": "jeonju",    "name": "전주 한옥마을","region": "호남", "lat": 35.8150, "lng": 127.1530, "desc": "한옥과 미식의 도시",     "area_cd": "52", "signgu_cd": "52111"},
    {"key": "yeosu",     "name": "여수",       "region": "호남", "lat": 34.7604, "lng": 127.6622, "desc": "밤바다와 섬 여행",       "area_cd": "46", "signgu_cd": "46130"},
    {"key": "gwangju",   "name": "광주 양림동", "region": "호남", "lat": 35.1440, "lng": 126.9160, "desc": "근대와 예술의 골목",     "area_cd": "29", "signgu_cd": "29155"},
    # 제주
    {"key": "jejucity",  "name": "제주시",     "region": "제주", "lat": 33.4996, "lng": 126.5312, "desc": "화산섬의 관문",         "area_cd": "50", "signgu_cd": "50110"},
    {"key": "seogwipo",  "name": "서귀포·중문", "region": "제주", "lat": 33.2496, "lng": 126.5600, "desc": "폭포와 리조트의 남쪽 해안","area_cd": "50", "signgu_cd": "50130"},
]

# 집중률(signgu, 오늘) 결과 캐시 — 하루 1회 호출로 트래픽 절약(일일 1000 제한).
_tats_cache: dict[str, dict[str, int]] = {}


def _norm(s: str) -> str:
    return "".join((s or "").split()).replace("(", "").replace(")", "")


def _tats_levels(area_cd: str, signgu_cd: str) -> dict[str, int]:
    """(오늘 기준) 시군구 관광지명 → 혼잡 레벨. 실패/키없음 시 빈 dict."""
    today = datetime.date.today().strftime("%Y%m%d")
    ck = f"{signgu_cd}:{today}"
    if ck in _tats_cache:
        return _tats_cache[ck]
    levels: dict[str, int] = {}
    try:
        rows = TatsClient().list_by_sigungu(area_cd, signgu_cd)
        for r in rows:
            if r["ymd"] != today:
                continue
            levels[_norm(r["name"])] = r["level"]
        if not levels:   # 오늘자가 없으면 가장 이른 날짜로
            for r in rows:
                levels.setdefault(_norm(r["name"]), r["level"])
    except (TatsError, Exception):
        levels = {}
    _tats_cache[ck] = levels
    return levels


def _match_level(levels: dict[str, int], title: str):
    """관광지명 정규화 매칭(정확 → 부분포함)."""
    if not levels or not title:
        return None
    key = _norm(title)
    if key in levels:
        return levels[key]
    for name, lvl in levels.items():
        if name and (name in key or key in name):
            return lvl
    return None


@router.get("/districts")
def list_districts() -> dict:
    return {"districts": DISTRICTS}


@router.get("/districts/{key}")
def district(key: str,
             radius_km: float = Query(3.0, ge=0.5, le=8.0),
             limit: int = Query(30, ge=1, le=50)) -> dict:
    """지역구 라인업 — 중심 좌표 반경 내 명소를 가까운 순으로."""
    d = next((x for x in DISTRICTS if x["key"] == key), None)
    if d is None:
        raise HTTPException(status_code=404, detail=f"unknown district: {key}")
    rows = repository.nearby_places(d["lat"], d["lng"], radius_km, limit)
    items = [_to_result(p) for p in rows]

    # 전국 집중률(TatsCnctrRateService)로 각 명소의 오늘 혼잡 레벨을 채운다(이름 매칭).
    levels = _tats_levels(d.get("area_cd"), d.get("signgu_cd")) \
        if d.get("signgu_cd") else {}
    for it in items:
        lvl = _match_level(levels, it["title"])
        if lvl:
            it["level"] = lvl
            it["quiet_score"] = quiet_score(lvl)
    return {"district": d, "places": items, "congestion_source": "tats" if levels else None}
