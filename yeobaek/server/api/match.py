"""POST /api/v1/match — 감성 쌍둥이 top-K (부록 B)."""
from __future__ import annotations
import time

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from ..config import settings
from ..db import repository
from ..services.match_service import find_twins, forecast_level

router = APIRouter(prefix="/api/v1", tags=["match"])


class MatchRequest(BaseModel):
    content_id: int
    radius_km: float = Field(default=settings.DEFAULT_RADIUS_KM, gt=0, le=50)
    top_k: int = Field(default=settings.DEFAULT_TOP_K, ge=1, le=20)
    arrival_time: str | None = None  # 선택: 예보 기준 시각(KST ISO). 없으면 '지금'.


@router.post("/match")
def match(req: MatchRequest) -> dict:
    source = repository.get_place(req.content_id)
    if not source:
        raise HTTPException(status_code=404, detail=f"content_id {req.content_id} not found")

    if req.arrival_time:
        from ..util import kst_iso_to_unix
        arrival = kst_iso_to_unix(req.arrival_time)
    else:
        arrival = int(time.time())

    twins = find_twins(source, req.radius_km, req.top_k)
    for t in twins:
        t["forecast_level"] = forecast_level(t.get("area_name"), arrival)

    return {
        "source": {
            "content_id": source["content_id"],
            "title": source["title"],
            "addr": source.get("addr"),
            "lat": source.get("lat"),
            "lng": source.get("lng"),
            "cat": source.get("cat"),
        },
        "twins": [
            {
                "content_id": t["content_id"],
                "title": t["title"],
                "similarity": t["similarity"],
                "forecast_level": t["forecast_level"],
                "dist_km": t["dist_km"],
            }
            for t in twins
        ],
    }
