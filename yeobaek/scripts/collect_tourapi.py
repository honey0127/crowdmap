"""1-1. TourAPI(한국관광공사) 서울 관광지 수집 → places 테이블 (절차서 Phase 1).

사용:
    export TOURAPI_KEY=발급키(디코딩)
    cd yeobaek && python scripts/collect_tourapi.py --pages 5 --rows 100

overview 없는 항목은 스킵/로깅한다(임베딩 품질 보호, 결정 E).
"""
from __future__ import annotations
import argparse
import sqlite3
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from server.config import settings                # noqa: E402
from server.db import repository                  # noqa: E402
from server.services.tourapi import TourAPIClient, TourAPIError  # noqa: E402


def _flt(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def _int(v):
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def _cat(item: dict) -> str:
    parts = [item.get("cat1"), item.get("cat2"), item.get("cat3")]
    return ">".join([p for p in parts if p])


# TourAPI 시·도 areaCode (전국 17개) — 1=서울 … 39=제주
ALL_AREA_CODES = [1, 2, 3, 4, 5, 6, 7, 8, 31, 32, 33, 34, 35, 36, 37, 38, 39]
AREA_NAMES = {
    1: "서울", 2: "인천", 3: "대전", 4: "대구", 5: "광주", 6: "부산", 7: "울산",
    8: "세종", 31: "경기", 32: "강원", 33: "충북", 34: "충남", 35: "경북",
    36: "경남", 37: "전북", 38: "전남", 39: "제주",
}


def upsert(con: sqlite3.Connection, row: dict) -> None:
    con.execute(
        "INSERT INTO places(content_id,title,addr,mapx,mapy,area_code,sigungu_code,cat,overview,updated_at)"
        " VALUES (:content_id,:title,:addr,:mapx,:mapy,:area_code,:sigungu_code,:cat,:overview,datetime('now'))"
        " ON CONFLICT(content_id) DO UPDATE SET"
        " title=excluded.title, addr=excluded.addr, mapx=excluded.mapx, mapy=excluded.mapy,"
        " area_code=excluded.area_code, sigungu_code=excluded.sigungu_code, cat=excluded.cat,"
        " overview=excluded.overview, updated_at=excluded.updated_at",
        row,
    )


def collect_area(client: TourAPIClient, con: sqlite3.Connection, area_code: int,
                 pages: int, rows: int, content_type_id, sleep: float,
                 need_overview: bool) -> tuple[int, int, int]:
    """한 시·도(area_code)의 pages 페이지를 수집. (saved, skipped, total) 누적 반환."""
    saved = skipped = total = 0
    label = AREA_NAMES.get(area_code, str(area_code))
    for page in range(1, pages + 1):
        try:
            items = client.area_based_list(
                area_code=area_code, page=page, rows=rows,
                content_type_id=content_type_id)
        except TourAPIError as e:
            print(f"[{label} p{page}] TourAPI 오류: {e}", file=sys.stderr)
            break
        if not items:
            print(f"[{label} p{page}] 결과 없음 — 이 지역 종료")
            break
        for it in items:
            total += 1
            cid = _int(it.get("contentid"))
            if cid is None:
                skipped += 1
                continue
            detail = {}
            overview = ""
            # overview 는 detailCommon 에서. 전국 대량 수집 시엔 생략 가능(--no-overview).
            if need_overview:
                try:
                    detail = client.detail_common(cid) or {}
                except TourAPIError:
                    detail = {}
                overview = (detail.get("overview") or "").strip()
                if not overview:
                    skipped += 1
                    time.sleep(sleep)
                    continue
            row = {
                "content_id": cid,
                "title": it.get("title") or detail.get("title") or "",
                "addr": it.get("addr1") or detail.get("addr1"),
                "mapx": _flt(it.get("mapx") or detail.get("mapx")),
                "mapy": _flt(it.get("mapy") or detail.get("mapy")),
                "area_code": _int(it.get("areacode")) or area_code,
                "sigungu_code": _int(it.get("sigungucode")),
                "cat": _cat(it) or _cat(detail),
                "overview": overview or None,
            }
            upsert(con, row)
            saved += 1
            if need_overview:
                time.sleep(sleep)
        con.commit()
        print(f"[{label} p{page}] 누적 saved={saved} skipped={skipped}")
    return saved, skipped, total


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--area-code", type=int, default=1, help="서울=1 (단일 지역)")
    ap.add_argument("--nationwide", action="store_true",
                    help="전국 17개 시·도 전체 수집(서울 중심 탈피)")
    ap.add_argument("--pages", type=int, default=5, help="지역당 페이지 수")
    ap.add_argument("--rows", type=int, default=100)
    ap.add_argument("--content-type-id", type=int, default=None,
                    help="예: 12=관광지, 14=문화시설, 25=여행코스")
    ap.add_argument("--no-overview", action="store_true",
                    help="상세 소개 호출 생략(대량 수집 속도↑, 임베딩 품질↓)")
    ap.add_argument("--sleep", type=float, default=0.2, help="호출 간 대기(초)")
    args = ap.parse_args()

    if not settings.TOURAPI_KEY:
        print("ERROR: TOURAPI_KEY 환경변수가 필요합니다.", file=sys.stderr)
        return 2

    repository.init_db()
    client = TourAPIClient()
    con = sqlite3.connect(settings.DB_PATH)

    area_codes = ALL_AREA_CODES if args.nationwide else [args.area_code]
    need_overview = not args.no_overview
    g_saved = g_skipped = g_total = 0
    try:
        for ac in area_codes:
            s, k, t = collect_area(client, con, ac, args.pages, args.rows,
                                    args.content_type_id, args.sleep, need_overview)
            g_saved += s; g_skipped += k; g_total += t
    finally:
        con.commit()
        con.close()

    scope = "전국" if args.nationwide else AREA_NAMES.get(args.area_code, str(args.area_code))
    print(f"\n완료[{scope}]: saved={g_saved}, skipped={g_skipped}, total={g_total} "
          f"→ {settings.DB_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
