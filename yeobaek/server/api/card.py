"""POST /api/v1/card — RAG 설득 카드 (부록 B, 결정 G)."""
from __future__ import annotations
import time

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel

from ..db import repository
from ..services.match_service import forecast_level
from ..services.rag import build_card

router = APIRouter(prefix="/api/v1", tags=["card"])


class CardRequest(BaseModel):
    source_id: int
    alt_id: int
    arrival_time: str | None = None  # 선택: 혼잡도 비교 기준 시각(KST ISO)


@router.post("/card")
def card(req: CardRequest) -> dict:
    source = repository.get_place(req.source_id)
    alt = repository.get_place(req.alt_id)
    if not source:
        raise HTTPException(status_code=404, detail=f"source_id {req.source_id} not found")
    if not alt:
        raise HTTPException(status_code=404, detail=f"alt_id {req.alt_id} not found")

    if req.arrival_time:
        from ..util import kst_iso_to_unix
        arrival = kst_iso_to_unix(req.arrival_time)
    else:
        arrival = int(time.time())

    src_area = repository.get_area_name(req.source_id)
    alt_area = repository.get_area_name(req.alt_id)
    source_level = forecast_level(src_area, arrival)
    alt_level = forecast_level(alt_area, arrival)

    return build_card(source, alt, source_level, alt_level)
