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


def main() -> int:
    repository.init_db()
    client = ye.SeoulCityDataForecastClient("")  # 좌표 매핑엔 키 불필요

    places = repository.all_places()
    if not places:
        print("places 가 비어 있습니다. 먼저 collect_tourapi.py 를 실행하세요.", file=sys.stderr)
        return 1

    con = sqlite3.connect(settings.DB_PATH)
    mapped = 0
    for p in places:
        name, dist_km, found = client.nearest_area(p["lat"], p["lng"])
        if not found:
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
    print(f"완료: {mapped}개 장소 → 서울 예보지점 매핑 (place_area_map)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
