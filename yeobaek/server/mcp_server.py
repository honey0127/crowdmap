"""여백 MCP 서버 — 여백의 혼잡 회피 엔진을 AI 에이전트가 쓸 수 있게 노출한다.

지금까지의 MCP 는 '여백이 남의 데이터를 가져오는' 방향이었다. 여기서는 방향을 뒤집어
**여백 자체가 도구가 된다**: Claude/ChatGPT 같은 AI 클라이언트가

    "경복궁 지금 붐비는데 비슷한 분위기의 한적한 곳 알려줘"
    "내일 오전 10시에 경복궁·국립중앙박물관·명동 도는 코스 짜줘"

라고 물으면, 이 서버가 여백의 예보·감성 쌍둥이·시간의존 스케줄러를 실행해 답한다.
→ 여백은 앱일 뿐 아니라 **다른 AI 서비스가 호출할 수 있는 관광 혼잡 회피 엔진**이 된다.

구현 노트:
  - MCP 는 JSON-RPC 2.0 이다. 외부 SDK 없이 필요한 메서드(initialize / tools/list /
    tools/call)만 직접 처리한다 — 배포 이미지에 의존성을 늘리지 않기 위해서다.
  - 도구 구현은 기존 FastAPI 라우트 핸들러를 **그대로 호출**한다(전부 동기 함수).
    로직을 복제하지 않으므로 앱과 MCP 의 답이 어긋날 수 없다.
    단, 핸들러의 기본값이 Query(...) 객체라 모든 인자를 명시적으로 넘긴다.
  - AI 는 content_id 를 모르므로 도구 입력은 **장소 이름**을 받고 내부에서 검색해 푼다.
"""
from __future__ import annotations

import json
from datetime import datetime, timedelta, timezone
from typing import Any, Callable

from fastapi import APIRouter, Request
from fastapi.responses import JSONResponse

from .api import match as match_api
from .api import places as places_api
from .api import schedule as schedule_api
from .api.match import MatchRequest
from .api.schedule import ScheduleRequest
from .db import repository

router = APIRouter(tags=["mcp"])

SERVER_NAME = "yeobaek"
SERVER_VERSION = "0.1.0"
DEFAULT_PROTOCOL = "2025-06-18"

KST = timezone(timedelta(hours=9))

_LEVEL_LABEL = {1: "여유", 2: "보통", 3: "약간 붐빔", 4: "붐빔"}


# ── 이름 → 장소 해석 ─────────────────────────────────────────────────────────
def _resolve_place(name: str) -> dict | None:
    """장소 이름으로 여백 DB 의 명소를 찾는다(부분일치 상위 1건)."""
    rows = repository.search_places(name, 5)
    if not rows:
        return None
    # 완전일치가 있으면 우선
    for r in rows:
        if (r.get("title") or "").strip() == name.strip():
            return r
    return rows[0]


def _need_place(name: str) -> tuple[dict | None, str | None]:
    p = _resolve_place(name)
    if p:
        return p, None
    return None, (f"'{name}' 을(를) 여백 데이터에서 찾지 못했습니다. "
                  f"search_places 로 정확한 이름을 먼저 확인해 보세요.")


def _lvl(level: Any) -> str:
    try:
        return _LEVEL_LABEL.get(int(level), "정보 없음")
    except (TypeError, ValueError):
        return "정보 없음"


# ── 도구 구현 ────────────────────────────────────────────────────────────────
def _t_search_places(args: dict) -> str:
    q = (args.get("query") or "").strip()
    if not q:
        return "query 가 필요합니다."
    res = places_api.search(q=q, limit=int(args.get("limit") or 10))["results"]
    if not res:
        return f"'{q}' 에 해당하는 장소가 없습니다."
    lines = [f"'{q}' 검색 결과 {len(res)}건:"]
    for r in res:
        bits = [r["title"]]
        if r.get("cat_label"):
            bits.append(r["cat_label"])
        if r.get("addr"):
            bits.append(r["addr"])
        lines.append("· " + " · ".join(bits))
    return "\n".join(lines)


def _t_find_quiet_alternative(args: dict) -> str:
    name = (args.get("place_name") or "").strip()
    p, err = _need_place(name)
    if err:
        return err
    top_k = int(args.get("top_k") or 3)
    radius = float(args.get("radius_km") or 5.0)
    out = match_api.match(MatchRequest(
        content_id=int(p["content_id"]), radius_km=radius, top_k=top_k,
        arrival_time=args.get("arrival_time")))
    twins = out.get("twins") or []
    src = out.get("source") or {}
    if not twins:
        return (f"'{src.get('title', name)}' 주변 {radius}km 안에서 대안을 찾지 못했습니다. "
                f"반경을 넓히거나 다른 장소로 시도해 보세요.")
    lines = [f"'{src.get('title', name)}' 대신 가 볼 만한 한적한 곳:"]
    for t in twins:
        basis = {"both": "감성 유사 + 관광공사 연관",
                 "embedding": "감성 유사도",
                 "related": "관광공사 연관 관광지"}.get(t.get("basis"), "추천")
        rank = f", 연관 {t['related_rank']}위" if t.get("related_rank") else ""
        lines.append(
            f"· {t['title']} — 혼잡 {_lvl(t.get('forecast_level'))}, "
            f"{t.get('dist_km')}km, 유사도 {t.get('similarity')} ({basis}{rank})")
    lines.append("\n근거: 서울 실시간 도시데이터 예보 + 한국관광공사 연관 관광지 + 자체 임베딩 유사도")
    return "\n".join(lines)


def _t_get_offpeak_hours(args: dict) -> str:
    name = (args.get("place_name") or "").strip()
    p, err = _need_place(name)
    if err:
        return err
    out = places_api.offpeak(int(p["content_id"]))
    best = out.get("best") or []
    if not best:
        return (f"'{p['title']}' 은(는) 예보 지점이 없어 시간대 추천이 어렵습니다"
                f" (서울 예보권 밖이거나 예보 미제공).")
    lines = [f"'{p['title']}' 덜 붐비는 시간 (출처: {out.get('source') or '서울 실시간 도시데이터'}):"]
    for h in best:
        t = datetime.fromtimestamp(int(h["unix"]), KST).strftime("%H:%M")
        extra = ""
        if h.get("ppltn_min") is not None and h.get("ppltn_max") is not None:
            extra = f", 예측 인구 {h['ppltn_min']}~{h['ppltn_max']}명"
        if h.get("historical"):
            extra += " (과거 같은 시간대 평균 추정)"
        lines.append(f"· {t} — {_lvl(h.get('level'))}, 한적함 {h.get('quiet_score')}{extra}")
    return "\n".join(lines)


def _t_plan_quiet_course(args: dict) -> str:
    names = args.get("place_names") or []
    if not isinstance(names, list) or not names:
        return "place_names 에 방문할 장소 이름을 1개 이상 넣어주세요."
    if len(names) > 8:
        return "한 번에 계획할 수 있는 장소는 최대 8곳입니다."

    stops, unresolved = [], []
    for n in names:
        p = _resolve_place(str(n))
        if p:
            stops.append(int(p["content_id"]))
        else:
            unresolved.append(str(n))
    if not stops:
        return f"입력한 장소를 하나도 찾지 못했습니다: {', '.join(unresolved)}"

    start = args.get("start_time")
    if not start:
        start = datetime.now(KST).strftime("%Y-%m-%dT%H:%M:00")
    try:
        out = schedule_api.schedule(ScheduleRequest(
            start_time=start, stops=stops,
            allow_substitution=bool(args.get("allow_substitution", True)),
            keep_order=bool(args.get("keep_order", False))))
    except Exception as e:            # 잘못된 시각 포맷 등
        return f"코스 생성 실패: {e}"

    ordered = out.get("ordered") or []
    lines = [f"{start} 출발 · 혼잡을 피해 재배치한 코스:"]
    for i, s in enumerate(ordered, 1):
        sub = " (혼잡해서 대체됨)" if s.get("substituted_from") else ""
        lines.append(f"{i}. {s['arrival']}  {s['title']} — {_lvl(s.get('forecast_level'))}{sub}")
    idx = out.get("yeobaek_index")
    tail = [f"\n혼잡 절감 {out.get('saved_congestion_pct', 0)}%"]
    if idx is not None:
        tail.append(f"여백지수 {idx}/100")
    tail.append(f"계산: {out.get('scheduler_mode', 'cpp')}")
    lines.append(" · ".join(tail))
    if unresolved:
        lines.append(f"※ 찾지 못한 장소는 제외했습니다: {', '.join(unresolved)}")
    return "\n".join(lines)


def _t_get_congestion(args: dict) -> str:
    name = (args.get("place_name") or "").strip()
    p, err = _need_place(name)
    if err:
        return err
    lat, lng = p.get("lat"), p.get("lng")
    if lat is None or lng is None:
        return f"'{p['title']}' 은(는) 좌표가 없어 혼잡도를 조회할 수 없습니다."
    rows = places_api.heatmap(lat=float(lat), lng=float(lng),
                              radius_km=0.5, limit=5)["results"]
    me = next((r for r in rows if r["content_id"] == p["content_id"]), None)
    if not me or me.get("level") is None:
        return (f"'{p['title']}' 의 현재 혼잡도를 알 수 없습니다"
                f" (서울 예보권 밖이거나 예보 미제공).")
    return (f"'{p['title']}' 현재 혼잡도: {_lvl(me['level'])}"
            f" (레벨 {me['level']}/4, 한적함 {me.get('quiet_score')})\n"
            f"출처: 서울 실시간 도시데이터 예보")


# ── 도구 정의(스키마) ────────────────────────────────────────────────────────
TOOLS: list[dict] = [
    {
        "name": "search_places",
        "description": "여백이 보유한 관광지를 이름으로 검색한다. 다른 도구에 넘길 정확한 "
                       "장소 이름을 확인할 때 먼저 쓴다.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "장소명(부분일치). 예: 북촌"},
                "limit": {"type": "integer", "description": "최대 건수(기본 10)"},
            },
            "required": ["query"],
        },
    },
    {
        "name": "find_quiet_alternative",
        "description": "붐비는 관광지 대신 갈 만한 '비슷한 분위기의 한적한 곳'을 추천한다. "
                       "서울 실시간 혼잡 예보 + 한국관광공사 연관 관광지 + 자체 감성 임베딩을 "
                       "결합해 고른다. 과대관광 분산이 필요할 때 쓴다.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "place_name": {"type": "string", "description": "붐비는 관광지 이름. 예: 북촌한옥마을"},
                "top_k": {"type": "integer", "description": "추천 개수(기본 3)"},
                "radius_km": {"type": "number", "description": "탐색 반경 km(기본 5)"},
                "arrival_time": {"type": "string",
                                 "description": "도착 예정 시각 KST ISO(예: 2026-10-11T14:00:00). 생략 시 현재"},
            },
            "required": ["place_name"],
        },
    },
    {
        "name": "get_offpeak_hours",
        "description": "특정 관광지가 덜 붐비는 시간대를 알려준다. 서울 실시간 도시데이터의 "
                       "시간대별 예측 인구를 근거로 하며, 예보창 밖이면 과거 같은 시간대 평균을 쓴다.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "place_name": {"type": "string", "description": "관광지 이름. 예: 경복궁"},
            },
            "required": ["place_name"],
        },
    },
    {
        "name": "plan_quiet_course",
        "description": "방문할 관광지 목록으로 '혼잡을 피한 하루 코스'를 만든다. 도착 시점의 "
                       "혼잡 예보를 반영해 방문 순서를 재배치하고, 너무 붐비는 곳은 비슷한 "
                       "분위기의 한적한 곳으로 대체한다.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "place_names": {"type": "array", "items": {"type": "string"},
                                "description": "방문할 장소 이름 목록(최대 8곳)"},
                "start_time": {"type": "string",
                               "description": "출발 시각 KST ISO(예: 2026-10-11T10:00:00). 생략 시 현재"},
                "allow_substitution": {"type": "boolean",
                                       "description": "붐비는 곳을 대안으로 바꿔도 되는지(기본 true)"},
                "keep_order": {"type": "boolean",
                               "description": "true 면 준 순서를 유지하고 도착 혼잡만 계산(기본 false)"},
            },
            "required": ["place_names"],
        },
    },
    {
        "name": "get_congestion",
        "description": "특정 관광지의 현재 혼잡도(1 여유 ~ 4 붐빔)를 조회한다.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "place_name": {"type": "string", "description": "관광지 이름"},
            },
            "required": ["place_name"],
        },
    },
]

_IMPL: dict[str, Callable[[dict], str]] = {
    "search_places": _t_search_places,
    "find_quiet_alternative": _t_find_quiet_alternative,
    "get_offpeak_hours": _t_get_offpeak_hours,
    "plan_quiet_course": _t_plan_quiet_course,
    "get_congestion": _t_get_congestion,
}


# ── JSON-RPC 처리 ────────────────────────────────────────────────────────────
def _result(req_id: Any, payload: dict) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "result": payload}


def _error(req_id: Any, code: int, message: str) -> dict:
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


def _handle(msg: dict) -> dict | None:
    """단일 JSON-RPC 메시지 처리. 알림(id 없음)은 None 을 돌려 204 로 응답한다."""
    method = msg.get("method")
    req_id = msg.get("id")
    params = msg.get("params") or {}

    if method == "initialize":
        # 클라이언트가 요구한 프로토콜 버전을 그대로 수용(모르는 값이면 기본값).
        ver = params.get("protocolVersion")
        if not isinstance(ver, str) or not ver:
            ver = DEFAULT_PROTOCOL
        return _result(req_id, {
            "protocolVersion": ver,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            "instructions": (
                "여백(Yeobaek)은 관광지 혼잡을 피해 여행 동선을 짜는 엔진입니다. "
                "장소 이름을 모르면 search_places 로 먼저 확인하세요. "
                "붐비는 곳의 대안은 find_quiet_alternative, 하루 코스는 plan_quiet_course 를 씁니다."
            ),
        })

    if method in ("notifications/initialized", "initialized"):
        return None

    if method == "ping":
        return _result(req_id, {})

    if method == "tools/list":
        return _result(req_id, {"tools": TOOLS})

    if method == "tools/call":
        name = params.get("name")
        args = params.get("arguments") or {}
        impl = _IMPL.get(name)
        if impl is None:
            return _error(req_id, -32602, f"unknown tool: {name}")
        try:
            text = impl(args if isinstance(args, dict) else {})
            is_err = False
        except Exception as e:                      # 도구 실패는 프로토콜 오류가 아니다
            text, is_err = f"도구 실행 중 오류: {e}", True
        return _result(req_id, {
            "content": [{"type": "text", "text": text}],
            "isError": is_err,
        })

    if req_id is None:                              # 처리할 수 없는 알림은 무시
        return None
    return _error(req_id, -32601, f"method not found: {method}")


@router.post("/mcp")
async def mcp_endpoint(request: Request):
    """MCP Streamable HTTP 엔드포인트(JSON-RPC 2.0)."""
    try:
        body = await request.json()
    except Exception:
        return JSONResponse(_error(None, -32700, "parse error"), status_code=400)

    # 배치 요청 지원
    if isinstance(body, list):
        out = [r for r in (_handle(m) for m in body if isinstance(m, dict)) if r]
        return JSONResponse(out) if out else JSONResponse(None, status_code=202)

    if not isinstance(body, dict):
        return JSONResponse(_error(None, -32600, "invalid request"), status_code=400)

    res = _handle(body)
    if res is None:
        return JSONResponse(None, status_code=202)
    return JSONResponse(res)


@router.get("/mcp")
def mcp_info() -> dict:
    """브라우저로 열어봤을 때의 안내(연결은 POST JSON-RPC 로 한다)."""
    return {
        "server": SERVER_NAME,
        "version": SERVER_VERSION,
        "protocol": DEFAULT_PROTOCOL,
        "transport": "streamable-http (POST /mcp, JSON-RPC 2.0)",
        "tools": [t["name"] for t in TOOLS],
    }
