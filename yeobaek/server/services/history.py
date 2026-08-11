"""D+1 역사 평균 폴백 (부록 A `forecast_cache` 활용).

서울 실시간 도시데이터의 FCST_PPLTN 은 약 12시간 앞까지만 예보한다. 그보다 먼
시각(예: "내일 오전 방문")을 물으면 엔진은 예보도 실시간도 못 찾아 중립값(레벨2)
으로 떨어지는데, 이건 "내일 여행" 시나리오에서 가장 자주 발생하는 경로라 매번
중립만 보여주면 신뢰를 잃는다.

대신 지금까지 실제로 관측된(라이브) 예보값을 `forecast_cache` 에 누적해두고,
같은 지점·같은 시간대(hour-of-day)의 과거 평균으로 대체한다. 표본이 쌓일수록
정확해지는 구조이며, 표본이 아직 없으면(신규 배포 초기) None 을 반환해
호출측이 기존 중립 폴백을 쓰게 한다 — 즉 이 모듈은 있으면 쓰고 없으면 조용히
비켜주는 순수 개선(strict improvement)이다.
"""
from __future__ import annotations
import sqlite3
import time

from ..config import settings
from ..util import KST_OFFSET

MIN_SAMPLES = 3  # 이보다 표본이 적으면 평균을 신뢰하지 않고 None(=기존 중립 폴백)


def _hour_of_day_kst(unix_ts: int) -> int:
    return time.gmtime(int(unix_ts) + KST_OFFSET).tm_hour


def _connect() -> sqlite3.Connection:
    return sqlite3.connect(settings.DB_PATH)


def record(area_name: str, level: int, ppltn_min: int | None, ppltn_max: int | None,
           fcst_unix: int) -> None:
    """실측(라이브) 예보 1건을 forecast_cache 에 적재 — 미래 D+1 폴백의 표본이 된다."""
    if not area_name or not level:
        return
    con = _connect()
    try:
        con.execute(
            "INSERT OR REPLACE INTO forecast_cache"
            "(seoul_area_name, fcst_time, congest_lvl, ppltn_min, ppltn_max, fetched_at)"
            " VALUES (?, ?, ?, ?, ?, ?)",
            (area_name, str(int(fcst_unix)), int(level),
             int(ppltn_min) if ppltn_min else None, int(ppltn_max) if ppltn_max else None,
             str(int(time.time()))),
        )
        con.commit()
    finally:
        con.close()


def historical_level(area_name: str, arrival_unix: int) -> int | None:
    """area_name 의 같은 시간대(hour-of-day, KST) 과거 관측 평균 레벨(반올림).
    표본이 MIN_SAMPLES 미만이면 None(호출측이 중립 폴백을 쓰게 둔다)."""
    target_hour = _hour_of_day_kst(arrival_unix)
    con = _connect()
    try:
        rows = con.execute(
            "SELECT fcst_time, congest_lvl FROM forecast_cache WHERE seoul_area_name = ?",
            (area_name,),
        ).fetchall()
    finally:
        con.close()

    levels = [lvl for t, lvl in rows if _hour_of_day_kst(int(t)) == target_hour]
    if len(levels) < MIN_SAMPLES:
        return None
    return round(sum(levels) / len(levels))
