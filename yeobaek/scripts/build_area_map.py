"""1-2. 장소 좌표 → 가장 가까운 서울 121 예보지점 매핑 → place_area_map (결정 C).

CrowdMap SeoulCityDataClient::findNearestArea 를 pybind(nearest_area)로 노출해 사용한다.
좌표 테이블만 쓰므로 SEOUL_API_KEY 없이도 동작한다.

사용:  cd yeobaek && python scripts/build_area_map.py
"""
from __future__ import annotations
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "server"))
sys.path.insert(0, str(ROOT / "engine" / "build"))

from server.config import settings   # noqa: E402
from server.db import repository      # noqa: E402

try:
    import yeobaek_engine as ye       # noqa: E402
except Exception as e:
    print(f"ERROR: yeobaek_engine import 실패({e}). engine/ 를 먼저 빌드하세요.", file=sys.stderr)
    raise SystemExit(2)


# 서울 예보지점과 이 거리(km) 안에 있는 장소만 매핑한다.
# 전국 수집 시 서울 밖 장소가 수백 km 떨어진 서울 지점의 혼잡도를 잘못 물려받지 않게 한다.
# (매핑이 없으면 예보는 중립'보통'으로 폴백 — 서울 밖에서는 이것이 올바른 동작)
MAX_MAP_DIST_KM = 4.0


def main() -> int:
    repository.init_db()
    client = ye.SeoulCityDataForecastClient("")  # 좌표 매핑엔 키 불필요

    places = repository.all_places()
    if not places:
        print("places 가 비어 있습니다. 먼저 collect_tourapi.py 를 실행하세요.", file=sys.stderr)
        return 1

    con = sqlite3.connect(settings.DB_PATH)
    mapped = far = 0
    for p in places:
        name, dist_km, found = client.nearest_area(p["lat"], p["lng"])
        if not found or dist_km > MAX_MAP_DIST_KM:
            far += 1                     # 서울 예보권 밖 → 매핑 안 함(중립 폴백)
            continue
        con.execute(
            "INSERT INTO place_area_map(content_id,seoul_area_name,dist_km) VALUES (?,?,?)"
            " ON CONFLICT(content_id) DO UPDATE SET seoul_area_name=excluded.seoul_area_name,"
            " dist_km=excluded.dist_km",
            (p["content_id"], name, round(float(dist_km), 3)),
        )
        mapped += 1
    con.commit()
    con.close()
    print(f"완료: {mapped}개 매핑(서울 예보권 ≤{MAX_MAP_DIST_KM}km), "
          f"{far}개 서울 밖 → 중립 폴백 (place_area_map)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
