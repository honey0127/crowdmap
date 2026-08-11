"""엔진(.so) 없을 때의 순수 파이썬 예보 폴백 (모듈2 대체 경로).

C++ 엔진이 없으면 여태 /schedule 이 통째로 503 이었다 — 배포 환경에 엔진 빌드가
실패하거나(아키텍처 불일치 등) 아직 안 붙었을 때 서비스가 완전히 멎는 게 문제다.
이 모듈은 SeoulCityDataForecastClient.h 의 동작(서울 실시간 도시데이터 호출 →
FCST_PPLTN 에서 도착시점에 가장 가까운 예보 선택 → 없으면 실시간 레벨 → 없으면
history.py 의 과거 평균 → 그래도 없으면 중립) 을 requests 로 그대로 재현한다.

C++ 판 대비 생략한 것(폴백이라 허용): 인근 예보지점으로의 2차 폴백(전체 121개
좌표 테이블이 C++ 쪽에만 있음), LRU 캐시 크기 제한(대신 TTL 만료 기반 단순 dict).
"""
from __future__ import annotations
import calendar
import time
import urllib.parse
from dataclasses import dataclass

import requests

from ..config import settings
from ..util import KST_OFFSET
from . import history

_CACHE: dict[str, tuple["PyForecastResult", float]] = {}   # key -> (result, expires_at)
_CACHE_TTL_OK = 600     # 실제 예보 성공 시(초)
_CACHE_TTL_FALLBACK = 60


@dataclass
class PyForecastResult:
    """engine.HistoricalForecast·pybind ForecastResult 와 같은 필드 모양(level/ppltn_min/
    ppltn_max/valid) — places.py 의 offpeak() 가 엔진 유무와 무관하게 다룰 수 있게 한다."""
    level: int
    ppltn_min: int = 0
    ppltn_max: int = 0
    valid: bool = True
    is_historical: bool = False


def _bucket_key(area_name: str, arrival_unix: int, bucket_sec: int = 3600) -> str:
    bucket = (int(arrival_unix) // bucket_sec) * bucket_sec
    return f"{area_name}|{bucket}"


def _seoul_level_to_int(label: str) -> int:
    return {"여유": 1, "보통": 2, "약간 붐빔": 3, "붐빔": 4}.get(label, 2)


def _fetch_citydata(area_name: str) -> dict | None:
    if not settings.SEOUL_API_KEY:
        return None
    url = (f"http://openapi.seoul.go.kr:8088/{settings.SEOUL_API_KEY}"
           f"/json/citydata_ppltn/1/1/{urllib.parse.quote(area_name)}")
    try:
        resp = requests.get(url, timeout=3)
        resp.raise_for_status()
        data = resp.json()
        rows = data.get("SeoulRtd.citydata_ppltn")
        if not rows:
            return None
        return rows[0]
    except Exception:
        return None


def forecast(area_name: str | None, arrival_unix: int) -> PyForecastResult:
    """도착시점 예보(레벨 + 원본 예측 인구 범위). 엔진 없을 때 /schedule·/match·
    /places(오프피크 등)가 공통으로 쓴다."""
    if not area_name:
        return PyForecastResult(level=2, valid=False)

    key = _bucket_key(area_name, arrival_unix)
    cached = _CACHE.get(key)
    if cached and cached[1] > time.time():
        return cached[0]

    row = _fetch_citydata(area_name)
    if row:
        fcst = row.get("FCST_PPLTN") or []
        best, best_delta = None, None
        for f in fcst:
            t = f.get("FCST_TIME")
            if not t:
                continue
            try:
                y, mo, d = int(t[0:4]), int(t[5:7]), int(t[8:10])
                h, mi = int(t[11:13]), int(t[14:16])
            except (ValueError, IndexError):
                continue
            # "YYYY-MM-DD HH:MM"(KST 벽시계) → UTC epoch.
            epoch = calendar.timegm((y, mo, d, h, mi, 0, 0, 0, 0)) - KST_OFFSET
            delta = abs(epoch - int(arrival_unix))
            if best_delta is None or delta < best_delta:
                best, best_delta = f, delta
        if best:
            result = PyForecastResult(
                level=_seoul_level_to_int(best.get("FCST_CONGEST_LVL", "보통")),
                ppltn_min=int(best.get("FCST_PPLTN_MIN") or 0),
                ppltn_max=int(best.get("FCST_PPLTN_MAX") or 0),
            )
            _CACHE[key] = (result, time.time() + _CACHE_TTL_OK)
            return result

        realtime_label = row.get("AREA_CONGEST_LVL")
        if realtime_label:
            result = PyForecastResult(level=_seoul_level_to_int(realtime_label))
            _CACHE[key] = (result, time.time() + _CACHE_TTL_FALLBACK)
            return result

    hist = history.historical_level(area_name, int(arrival_unix))
    if hist is not None:
        return PyForecastResult(level=hist, is_historical=True)

    return PyForecastResult(level=2, valid=False)


def forecast_level(area_name: str | None, arrival_unix: int) -> int:
    """도착시점 예보 레벨(1~4)만 필요한 호출부(스케줄러 등)용 얇은 래퍼."""
    return forecast(area_name, arrival_unix).level
