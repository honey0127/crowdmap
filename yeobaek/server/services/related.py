"""관광지 별 연관 관광지 정보(한국관광공사 관광 빅데이터) 클라이언트.

'이 관광지를 찾은 사람들이 함께 찾은 관광지'를 공사가 집계해 제공한다.
여백은 이걸 **감성 쌍둥이(대안 추천)의 공식 근거**로 쓴다:

    후보 = e5 임베딩 유사도(자체 계산)  ⊕  공사 연관 관광지(공식 데이터)

임베딩만 쓰면 "왜 이 대안인가"의 근거가 우리 모델뿐이지만, 연관 관광지를 결합하면
실제 관광객 행동 데이터에 기반했다고 말할 수 있다.

오퍼레이션(공공데이터포털 기준): 지역기반관광정보(areaBasedList), 키워드검색(searchKeyword).
※ 엔드포인트는 기관이 버전을 올릴 수 있으므로 env(RLTE_ENDPOINT)로 교체 가능하게 둔다.
"""
from __future__ import annotations

import json
import time
import urllib.parse
import urllib.request
from typing import Optional

from ..config import settings


class RelatedError(Exception):
    pass


def _norm(s: str) -> str:
    """관광지명 비교용 정규화 — 공백/괄호 제거(공사 표기와 TourAPI 표기가 조금씩 다르다)."""
    return "".join((s or "").split()).replace("(", "").replace(")", "")


class RelatedClient:
    """연관 관광지 조회. 키가 없거나 실패하면 예외 — 호출측이 조용히 폴백한다."""

    def __init__(self, key: Optional[str] = None, endpoint: Optional[str] = None):
        self.key = key if key is not None else settings.RLTE_API_KEY
        self.endpoint = (endpoint or settings.RLTE_ENDPOINT).rstrip("/")

    # /schedule 은 stop 마다 find_twins 를 부르므로(최대 8회) 타임아웃이 길면
    # 한 번의 요청이 통째로 느려진다. 짧게 끊고 실패는 폴백으로 흘린다.
    TIMEOUT_SEC = 4.0

    def _call(self, op: str, params: dict) -> dict:
        if not self.key:
            raise RelatedError("RLTE_API_KEY 미설정")
        full = {**params, "serviceKey": self.key, "MobileOS": "ETC",
                "MobileApp": "yeobaek", "_type": "json"}
        url = f"{self.endpoint}/{op}?" + urllib.parse.urlencode(full, safe="")
        req = urllib.request.Request(url, headers={"User-Agent": "yeobaek"})
        try:
            with urllib.request.urlopen(req, timeout=self.TIMEOUT_SEC) as r:
                body = r.read().decode("utf-8", "replace")
            return json.loads(body)
        except Exception as e:
            raise RelatedError(f"요청/파싱 실패: {e}")

    @staticmethod
    def _items(j: dict) -> list[dict]:
        header = j.get("response", {}).get("header", {})
        rc = header.get("resultCode")
        if rc not in ("0000", None):
            raise RelatedError(header.get("resultMsg", rc))
        items = j.get("response", {}).get("body", {}).get("items", {})
        item = items.get("item") if isinstance(items, dict) else None
        if item is None:
            return []
        return [item] if isinstance(item, dict) else list(item)

    @staticmethod
    def _to_row(it: dict) -> Optional[dict]:
        """공사 응답 → {name, rank, region}. 필드명이 버전에 따라 달라 여러 후보를 본다."""
        name = (it.get("rlteTatsNm") or it.get("rlteBsicAdr")
                or it.get("rlteNm") or it.get("tAtsNm") or "").strip()
        if not name:
            return None
        rank_raw = it.get("rlteRank") or it.get("rank")
        try:
            rank = int(rank_raw)
        except (TypeError, ValueError):
            rank = None
        return {
            "name": name,
            "rank": rank,
            "region": (it.get("rlteRegnNm") or it.get("areaNm") or "").strip() or None,
            "category": (it.get("rlteCtgryMclsNm") or it.get("rlteCtgrySclsNm") or "").strip() or None,
        }

    def by_keyword(self, keyword: str, rows: int = 20) -> list[dict]:
        """관광지명으로 연관 관광지 목록. 여백은 이 경로를 주로 쓴다(장소명 기반)."""
        j = self._call("searchKeyword", {
            "keyword": keyword, "numOfRows": str(rows), "pageNo": "1"})
        out = [r for r in (self._to_row(i) for i in self._items(j)) if r]
        # 순위가 있으면 순위순, 없으면 원래 순서 유지
        out.sort(key=lambda r: (r["rank"] is None, r["rank"] or 0))
        return out

    def by_area(self, area_cd: str, signgu_cd: str, rows: int = 100) -> list[dict]:
        """시군구 단위 연관 관광지 목록(지역 라인업 보강용)."""
        j = self._call("areaBasedList", {
            "areaCd": str(area_cd), "signguCd": str(signgu_cd),
            "numOfRows": str(rows), "pageNo": "1"})
        return [r for r in (self._to_row(i) for i in self._items(j)) if r]


# ── 조회 결과 캐시 ────────────────────────────────────────────────────────────
# 일일 트래픽이 1,000건(개발계정)이라 같은 장소를 반복 조회하면 금방 소진된다.
# 연관 관광지는 자주 바뀌지 않으므로 프로세스 메모리에 캐싱한다.
_cache: dict[str, list[dict]] = {}

# 서킷 브레이커: 키 미설정·쿼터 초과·장애가 났는데 계속 호출하면 /schedule 이
# stop 수만큼 타임아웃을 먹는다. 연속 실패가 쌓이면 한동안 아예 시도하지 않는다.
_MAX_FAILS = 3
_COOLDOWN_SEC = 300
_fails = 0
_blocked_until = 0.0


def _circuit_open() -> bool:
    return _fails >= _MAX_FAILS and time.time() < _blocked_until


def related_names(title: str) -> list[dict]:
    """장소명 → 연관 관광지 목록(실패 시 빈 리스트, 서비스는 계속 동작)."""
    global _fails, _blocked_until
    key = _norm(title)
    if key in _cache:
        return _cache[key]
    if _circuit_open():
        return []
    try:
        rows = RelatedClient().by_keyword(title)
        _fails = 0                      # 성공하면 회로 복구
    except Exception:
        rows = []                       # 키 미설정·쿼터 초과·장애 — 임베딩 단독으로 폴백
        _fails += 1
        if _fails >= _MAX_FAILS:
            _blocked_until = time.time() + _COOLDOWN_SEC
    _cache[key] = rows
    return rows


def status() -> dict:
    """/health 노출용 — 연관 관광지 연동이 실제로 살아있는지 확인."""
    return {
        "configured": bool(settings.RLTE_API_KEY),
        "endpoint": settings.RLTE_ENDPOINT,
        "cached_titles": len(_cache),
        "circuit_open": _circuit_open(),
    }


def rank_boost(related: list[dict], candidate_title: str) -> float:
    """후보가 연관 관광지 목록에 있으면 0<boost<=1 (상위일수록 큼). 없으면 0.

    상위 1위 → 1.0, 그 뒤로 완만히 감소. 순위 정보가 없으면 등장만으로 0.5.
    """
    if not related or not candidate_title:
        return 0.0
    key = _norm(candidate_title)
    for i, r in enumerate(related):
        rn = _norm(r["name"])
        if not rn:
            continue
        if rn == key or rn in key or key in rn:
            rank = r["rank"] if r["rank"] else (i + 1)
            return 1.0 / (1.0 + 0.15 * max(0, rank - 1))
    return 0.0
