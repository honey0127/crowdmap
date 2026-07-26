"""한국관광공사 TourAPI 클라이언트 (절차서 심사 필수 활용 · 0-2/1-1).

areaBasedList (지역기반 관광정보) + detailCommon (공통정보=overview) 를 감싼다.
응답 스키마의 nested items(item) 를 평탄화해서 dict 리스트로 돌려준다.

기본 엔드포인트는 KorService2. 구버전(KorService1)을 쓰려면 base_url 을 바꾼다.
serviceKey 는 환경변수(TOURAPI_KEY)로만 주입한다.
"""
from __future__ import annotations
from typing import Any, Optional

import requests

from ..config import settings

DEFAULT_BASE = "https://apis.data.go.kr/B551011/KorService2"


class TourAPIError(RuntimeError):
    pass


class TourAPIClient:
    def __init__(self, service_key: Optional[str] = None, base_url: str = DEFAULT_BASE,
                 mobile_app: str = "yeobaek", timeout: float = 10.0):
        self.key = service_key if service_key is not None else settings.TOURAPI_KEY
        self.base = base_url.rstrip("/")
        self.mobile_app = mobile_app
        self.timeout = timeout

    def _common_params(self) -> dict[str, Any]:
        # serviceKey 는 requests 가 재인코딩하지 않도록 디코딩된 키를 그대로 사용한다.
        return {
            "serviceKey": self.key,
            "MobileOS": "ETC",
            "MobileApp": self.mobile_app,
            "_type": "json",
        }

    def _get(self, path: str, params: dict[str, Any]) -> list[dict]:
        p = self._common_params()
        p.update(params)
        r = requests.get(f"{self.base}/{path}", params=p, timeout=self.timeout)
        r.raise_for_status()
        try:
            body = r.json()
        except ValueError:
            raise TourAPIError(f"비JSON 응답({path}): {r.text[:200]}")
        # response.body.items.item → list | dict | ""(빈)
        try:
            items = body["response"]["body"]["items"]
        except (KeyError, TypeError):
            header = body.get("response", {}).get("header", {})
            raise TourAPIError(f"TourAPI 오류({path}): {header}")
        if items in ("", None):
            return []
        item = items.get("item", []) if isinstance(items, dict) else items
        if isinstance(item, dict):
            item = [item]
        return item

    def area_based_list(self, area_code: int = 1, page: int = 1, rows: int = 100,
                        content_type_id: Optional[int] = None,
                        arrange: str = "C") -> list[dict]:
        """지역기반 관광지 목록. arrange=C: 수정일순(대표이미지 포함)."""
        # KorService2 규격: listYN 없음(=목록 기본). arrange 로 정렬.
        params: dict[str, Any] = {
            "areaCode": area_code,
            "numOfRows": rows,
            "pageNo": page,
            "arrange": arrange,
        }
        if content_type_id is not None:
            params["contentTypeId"] = content_type_id
        return self._get("areaBasedList2", params)

    def detail_common(self, content_id: int) -> Optional[dict]:
        """공통정보(overview 등). KorService2 는 *YN 플래그 없이 기본 반환한다.
        좌표(mapx/mapy)·카테고리(cat1~3)는 areaBasedList2 결과에 이미 포함된다."""
        items = self._get("detailCommon2", {"contentId": content_id})
        return items[0] if items else None
