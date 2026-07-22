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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--area-code", type=int, default=1, help="서울=1")
    ap.add_argument("--pages", type=int, default=5)
    ap.add_argument("--rows", type=int, default=100)
    ap.add_argument("--content-type-id", type=int, default=None,
                    help="예: 12=관광지, 14=문화시설, 25=여행코스")
    ap.add_argument("--sleep", type=float, default=0.2, help="호출 간 대기(초)")
    args = ap.parse_args()

    if not settings.TOURAPI_KEY:
        print("ERROR: TOURAPI_KEY 환경변수가 필요합니다.", file=sys.stderr)
        return 2

    repository.init_db()
    client = TourAPIClient()
    con = sqlite3.connect(settings.DB_PATH)

    total, skipped, saved = 0, 0, 0
    try:
        for page in range(1, args.pages + 1):
            try:
                items = client.area_based_list(
                    area_code=args.area_code, page=page, rows=args.rows,
                    content_type_id=args.content_type_id)
            except TourAPIError as e:
                print(f"[page {page}] TourAPI 오류: {e}", file=sys.stderr)
                break
            if not items:
                print(f"[page {page}] 결과 없음 — 종료")
                break

            for it in items:
                total += 1
                cid = _int(it.get("contentid"))
                if cid is None:
                    skipped += 1
                    continue
                # overview 는 detailCommon 에서 가져온다
                try:
                    detail = client.detail_common(cid) or {}
                except TourAPIError:
                    detail = {}
                overview = (detail.get("overview") or "").strip()
                if not overview:
                    skipped += 1
                    print(f"  skip content_id={cid} ({it.get('title')}): overview 없음")
                    time.sleep(args.sleep)
                    continue
                row = {
                    "content_id": cid,
                    "title": it.get("title") or detail.get("title") or "",
                    "addr": it.get("addr1") or detail.get("addr1"),
                    "mapx": _flt(it.get("mapx") or detail.get("mapx")),
                    "mapy": _flt(it.get("mapy") or detail.get("mapy")),
                    "area_code": _int(it.get("areacode")),
                    "sigungu_code": _int(it.get("sigungucode")),
                    "cat": _cat(it) or _cat(detail),
                    "overview": overview,
                }
                upsert(con, row)
                saved += 1
                time.sleep(args.sleep)
            con.commit()
            print(f"[page {page}] 누적 saved={saved} skipped={skipped} total={total}")
    finally:
        con.commit()
        con.close()

    print(f"\n완료: saved={saved}, skipped={skipped}, total={total} → {settings.DB_PATH}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
