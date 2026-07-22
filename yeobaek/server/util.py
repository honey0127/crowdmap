"""서버 공통 유틸: 시간 변환(KST), 거리, 카테고리 dwell."""
from __future__ import annotations
import calendar
import math
import time
from datetime import datetime

KST_OFFSET = 9 * 3600  # 서울 = UTC+9


def kst_iso_to_unix(iso: str) -> int:
    """'2026-10-11T10:00:00'(KST 벽시계) → UTC epoch seconds."""
    dt = datetime.fromisoformat(iso)
    # timetuple 을 UTC 로 간주해 epoch 화한 뒤 KST 보정
    return calendar.timegm(dt.timetuple()) - KST_OFFSET


def unix_to_kst_hhmm(unix_ts: int) -> str:
    """UTC epoch → KST 'HH:MM'."""
    lt = time.gmtime(int(unix_ts) + KST_OFFSET)
    return time.strftime("%H:%M", lt)


def haversine_km(lat1: float, lng1: float, lat2: float, lng2: float) -> float:
    R = 6371.0088
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlmb = math.radians(lng2 - lng1)
    a = math.sin(dphi / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dlmb / 2) ** 2
    return 2 * R * math.asin(min(1.0, math.sqrt(a)))


# 카테고리(대분류 코드/이름) → 기본 체류시간(초). 결정 D 근사.
_DWELL_BY_CAT = {
    "A01": 5400,   # 자연
    "A02": 5400,   # 인문(문화/역사) — 궁궐 등
    "A03": 7200,   # 레포츠
    "A04": 3600,   # 쇼핑
    "A05": 4500,   # 음식
    "자연": 5400, "역사": 5400, "문화": 5400, "쇼핑": 3600, "음식": 4500,
}


def dwell_for_category(cat: str | None, default_sec: int) -> int:
    if not cat:
        return default_sec
    head = cat.split(">")[0].strip()
    return _DWELL_BY_CAT.get(head, default_sec)
