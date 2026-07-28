"""SQLite 리포지토리 — places / place_area_map 읽기 (절차서 3-4).

읽기 위주이며 커넥션은 요청마다 열고 닫는다(sqlite 는 가볍다).
서버 부팅 시 all_places() 로 C++ SpatialIndex 를 적재한다.
"""
from __future__ import annotations
import sqlite3
from pathlib import Path
from typing import Iterable, Optional

from ..config import settings


def _connect() -> sqlite3.Connection:
    conn = sqlite3.connect(settings.DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


def init_db(schema_path: Optional[str] = None) -> None:
    """스키마 적용(멱등). schema.sql 을 실행한다."""
    schema_path = schema_path or str(Path(__file__).with_name("schema.sql"))
    with open(schema_path, "r", encoding="utf-8") as f:
        sql = f.read()
    conn = _connect()
    try:
        conn.executescript(sql)
        conn.commit()
    finally:
        conn.close()


def _row_to_place(r: sqlite3.Row) -> dict:
    return {
        "content_id": r["content_id"],
        "title": r["title"],
        "addr": r["addr"],
        "lng": r["mapx"],
        "lat": r["mapy"],
        "area_code": r["area_code"],
        "sigungu_code": r["sigungu_code"],
        "cat": r["cat"],
        "overview": r["overview"],
    }


def get_place(content_id: int) -> Optional[dict]:
    conn = _connect()
    try:
        r = conn.execute("SELECT * FROM places WHERE content_id=?", (content_id,)).fetchone()
        return _row_to_place(r) if r else None
    finally:
        conn.close()


def get_places(ids: Iterable[int]) -> dict[int, dict]:
    ids = list(ids)
    if not ids:
        return {}
    conn = _connect()
    try:
        qs = ",".join("?" * len(ids))
        rows = conn.execute(f"SELECT * FROM places WHERE content_id IN ({qs})", ids).fetchall()
        return {r["content_id"]: _row_to_place(r) for r in rows}
    finally:
        conn.close()


def all_places() -> list[dict]:
    """SpatialIndex 적재용: 좌표가 있는 모든 장소."""
    conn = _connect()
    try:
        rows = conn.execute(
            "SELECT * FROM places WHERE mapx IS NOT NULL AND mapy IS NOT NULL"
        ).fetchall()
        return [_row_to_place(r) for r in rows]
    finally:
        conn.close()


def get_area_map(ids: Iterable[int]) -> dict[int, tuple[str, float]]:
    """content_id → (seoul_area_name, dist_km)"""
    ids = list(ids)
    if not ids:
        return {}
    conn = _connect()
    try:
        qs = ",".join("?" * len(ids))
        rows = conn.execute(
            f"SELECT content_id, seoul_area_name, dist_km FROM place_area_map "
            f"WHERE content_id IN ({qs})",
            ids,
        ).fetchall()
        return {r["content_id"]: (r["seoul_area_name"], r["dist_km"]) for r in rows}
    finally:
        conn.close()


def get_area_name(content_id: int) -> Optional[str]:
    m = get_area_map([content_id])
    return m[content_id][0] if content_id in m else None


def search_places(q: str, limit: int = 20) -> list[dict]:
    """제목 부분일치 검색(이름으로 장소 추가하는 실사용 흐름). 좌표 있는 장소만."""
    conn = _connect()
    try:
        rows = conn.execute(
            "SELECT * FROM places WHERE title LIKE ? "
            "AND mapx IS NOT NULL AND mapy IS NOT NULL "
            "ORDER BY length(title), title LIMIT ?",
            (f"%{q}%", int(limit)),
        ).fetchall()
        return [_row_to_place(r) for r in rows]
    finally:
        conn.close()
