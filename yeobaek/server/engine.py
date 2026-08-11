"""C++ 엔진(yeobaek_engine) 로더 & 상태 보관.

부팅 시:
  1) yeobaek_engine 임포트 (.so 는 server/ 옆 또는 engine/build 에 위치)
  2) SpatialIndex 에 places 좌표 적재 (절차서 1-4)
  3) SeoulCityDataForecastClient / ForecastProvider / Scheduler 준비

.so 가 없으면 EngineState.available=False 로 두고, 라우터가 503 을 명확히 반환한다.
"""
from __future__ import annotations
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from .config import settings
from .db import repository
from .services import history


@dataclass
class HistoricalForecast:
    """D+1 폴백용 스탠드인 — pybind ForecastResult 와 같은 필드 모양(level/ppltn_min/max/valid).
    실측이 아니라 과거 같은 시간대 관측 평균이므로 fcst_unix=0(실측 아님을 표시)."""
    level: int
    fcst_unix: int = 0
    ppltn_min: int = 0
    ppltn_max: int = 0
    valid: bool = True
    is_historical: bool = True

# .so 탐색 경로: server/ (빌드 복사본) 과 engine/build/
_HERE = Path(__file__).resolve().parent
for _p in (_HERE, _HERE.parent / "engine" / "build"):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))

# Windows: Python 3.8+ 부터 확장 모듈 DLL 탐색에 PATH 를 자동으로 쓰지 않는다.
# yeobaek_engine(.pyd) 은 MSYS2 ucrt64 툴체인(libcurl 등)에 링크되므로 그 bin 을
# 명시적으로 등록해야 임포트 시 DLL load failed 가 나지 않는다.
if os.name == "nt" and hasattr(os, "add_dll_directory"):
    for _dll_dir in (Path(r"C:\msys64\ucrt64\bin"), Path(r"C:\msys64\mingw64\bin")):
        if _dll_dir.is_dir():
            try:
                os.add_dll_directory(str(_dll_dir))
            except OSError:
                pass

try:
    import yeobaek_engine as ye  # type: ignore
    _IMPORT_ERR = None
except Exception as e:  # pragma: no cover - 환경 의존
    ye = None  # type: ignore
    _IMPORT_ERR = e


class EngineState:
    def __init__(self) -> None:
        self.available = False
        self.import_error: Optional[str] = None if ye else repr(_IMPORT_ERR)
        self.spatial = None
        self.client = None
        self.provider = None
        self.scheduler = None
        self.places_loaded = 0

    def init(self) -> None:
        if ye is None:
            self.available = False
            return
        # 예보 클라이언트/프로바이더/스케줄러
        self.client = ye.SeoulCityDataForecastClient(settings.SEOUL_API_KEY)
        self.provider = ye.ForecastProvider(self.client)
        self.scheduler = ye.Scheduler(
            self.provider, high_congestion_level=settings.HIGH_CONGESTION_LEVEL)

        # SpatialIndex 적재
        self.spatial = ye.SpatialIndex()
        pts = [
            (int(p["content_id"]), float(p["lat"]), float(p["lng"]))
            for p in repository.all_places()
            if p["lat"] is not None and p["lng"] is not None
        ]
        if pts:
            self.spatial.add_many(pts)
        self.places_loaded = len(pts)
        self.available = True

    # ── 편의 래퍼 ──
    def query_radius(self, lat: float, lng: float, radius_km: float) -> list[int]:
        return list(self.spatial.query_radius(lat, lng, radius_km))

    def forecast(self, area_name: str, arrival_unix: int):
        """도착시점 예보. 서울 API 의 예보창(~12h)을 벗어나 엔진이 아무것도 못 찾으면
        (D+1 등) 같은 지점·같은 시간대의 과거 관측 평균으로 대체한다(모듈2 보강).
        실측이 나오면 그 값을 forecast_cache 에 적재해 다음 D+1 폴백의 표본을 늘린다."""
        fc = self.provider.get(area_name, int(arrival_unix))
        if fc.valid and fc.fcst_unix > 0:
            history.record(area_name, fc.level, fc.ppltn_min, fc.ppltn_max, fc.fcst_unix)
            return fc
        if not fc.valid:
            hist_level = history.historical_level(area_name, int(arrival_unix))
            if hist_level is not None:
                return HistoricalForecast(level=hist_level)
        return fc

    def resolve_now(self, lat: float, lng: float):
        """(lat,lng) 실시간 현재 혼잡 -> (level, ratio, valid) 또는 None. 모듈4 급증 감지용."""
        if not self.available or self.client is None:
            return None
        try:
            level, ratio, valid = self.client.resolve_now(float(lat), float(lng))
        except Exception:
            return None
        return (int(level), float(ratio), bool(valid)) if valid else None

    def nearest_area(self, lat: float, lng: float):
        """(lat,lng) 최근접 서울 예보지점 -> (name, dist_km) 또는 None(엔진 부재/미발견)."""
        if not self.available or self.client is None:
            return None
        try:
            name, dist, found = self.client.nearest_area(float(lat), float(lng))
        except Exception:
            return None
        return (name, dist) if found else None

    def make_stop(self, content_id, lat, lng, area_name, dwell_sec, twins=None):
        s = ye.SchedStop()
        s.content_id = int(content_id)
        s.lat = float(lat)
        s.lng = float(lng)
        s.area_name = area_name or ""
        s.dwell_sec = int(dwell_sec)
        if twins:
            tw_objs = []
            for t in twins:
                tw = ye.TwinCandidate()
                tw.content_id = int(t["content_id"])
                tw.lat = float(t["lat"])
                tw.lng = float(t["lng"])
                tw.area_name = t.get("area_name") or ""
                tw.similarity = float(t.get("similarity", 0.0))
                tw_objs.append(tw)
            s.twins = tw_objs
        return s

    def weights(self, congestion: float, distance: float, similarity: float):
        return ye.SchedWeights(congestion, distance, similarity)

    def optimize(self, stops, start_unix: int, weights, allow_substitution: bool,
                 keep_order: bool = False):
        return self.scheduler.optimize(
            stops, int(start_unix), weights, allow_substitution, keep_order)


engine_state = EngineState()
