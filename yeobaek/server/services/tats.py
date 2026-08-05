"""전국 관광지 집중률 예측(한국관광공사 TatsCnctrRateService) 클라이언트.

tatsCnctrRatedList: (areaCd, signguCd) 필수 → 향후 30일 관광지별 **일별 집중률**(cnctrRate 0~100).
서울 밖 전국을 커버하는 혼잡 소스. 집중률(%)을 여백 혼잡 레벨(1~4)로 버킷팅한다.

주의: 이 API 는 '일별' 예측이라 서울 실시간 도시데이터('시간별')와 입도가 다르다.
      → 도착 '날짜'의 집중률로 그 방문의 혼잡 레벨을 준다.
"""
from __future__ import annotations
import json
import urllib.parse
import urllib.request
from typing import Optional

from ..config import settings


def rate_to_level(rate: float) -> int:
    """집중률(%) → 혼잡 레벨 1~4. (튜닝 가능한 임계)"""
    if rate < 30:
        return 1        # 여유
    if rate < 60:
        return 2        # 보통
    if rate < 85:
        return 3        # 약간 붐빔
    return 4            # 붐빔


class TatsError(Exception):
    pass


class TatsClient:
    def __init__(self, key: Optional[str] = None, endpoint: Optional[str] = None):
        self.key = key if key is not None else settings.TATS_API_KEY
        self.endpoint = endpoint or settings.TATS_ENDPOINT

    def _call(self, params: dict) -> str:
        full = {**params, "serviceKey": self.key, "MobileOS": "ETC",
                "MobileApp": "yeobaek", "_type": "json"}
        url = self.endpoint + "?" + urllib.parse.urlencode(full, safe="")
        req = urllib.request.Request(url, headers={"User-Agent": "yeobaek"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode("utf-8", "replace")

    def list_by_sigungu(self, area_cd: str, signgu_cd: str,
                        base_ymd: Optional[str] = None, rows: int = 1000) -> list[dict]:
        """(area_cd, signgu_cd) 의 관광지 집중률 목록. base_ymd 없으면 30일 전체."""
        if not self.key:
            raise TatsError("TATS_API_KEY 미설정")
        p = {"areaCd": str(area_cd), "signguCd": str(signgu_cd),
             "numOfRows": str(rows), "pageNo": "1"}
        if base_ymd:
            p["baseYmd"] = str(base_ymd)
        try:
            body = self._call(p)
            j = json.loads(body)
        except Exception as e:
            raise TatsError(f"요청/파싱 실패: {e}")
        header = j.get("response", {}).get("header", {})
        rc = header.get("resultCode")
        if rc not in ("0000", None):
            raise TatsError(header.get("resultMsg", rc))
        items = j.get("response", {}).get("body", {}).get("items", {})
        item = items.get("item") if isinstance(items, dict) else None
        if item is None:
            return []
        if isinstance(item, dict):
            item = [item]
        out = []
        for it in item:
            try:
                rate = float(it.get("cnctrRate"))
            except (TypeError, ValueError):
                continue
            out.append({
                "ymd": it.get("baseYmd"),
                "area_cd": it.get("areaCd"), "area_nm": it.get("areaNm"),
                "signgu_cd": it.get("signguCd"), "signgu_nm": it.get("signguNm"),
                "name": it.get("tAtsNm"),
                "rate": round(rate, 2), "level": rate_to_level(rate),
            })
        return out
