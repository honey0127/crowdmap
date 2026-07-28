"""GET /api/v1/places/search — 이름으로 장소 검색(앱에서 방문지 추가용)."""
from __future__ import annotations

from fastapi import APIRouter, Query

from ..db import repository
from ..services.rag import cat_label

router = APIRouter(prefix="/api/v1", tags=["places"])


def _deepest_cat(cat: str | None) -> str | None:
    if not cat:
        return None
    parts = [c.strip() for c in cat.split(">") if c.strip()]
    return parts[-1] if parts else None


@router.get("/places/search")
def search(q: str = Query(..., min_length=1, description="장소명 부분일치"),
           limit: int = Query(20, ge=1, le=50)) -> dict:
    rows = repository.search_places(q, limit)
    return {
        "results": [
            {
                "content_id": p["content_id"],
                "title": p["title"],
                "addr": p.get("addr"),
                "cat_label": cat_label(_deepest_cat(p.get("cat"))),
                "lat": p.get("lat"),   # 지도 마커용(홈 화면)
                "lng": p.get("lng"),
            }
            for p in rows
        ]
    }
