"""배포 스모크 테스트 — 배포된 여백 서버가 실제로 살아있는지 한 번에 점검한다.

앱 API 와 MCP 서버를 모두 확인한다. 표준 라이브러리만 쓰므로 어디서든 바로 돌아간다.

사용:
    python scripts/smoke_deploy.py                       # http://localhost:8000
    python scripts/smoke_deploy.py https://<앱>.fly.dev  # 배포 서버

종료 코드: 전부 통과 0, 하나라도 실패 1 (CI 에 물릴 수 있다).
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timedelta, timezone

KST = timezone(timedelta(hours=9))
TIMEOUT = 30            # 무료 티어 콜드 스타트를 고려해 넉넉히

_results: list[tuple[bool, str, str]] = []


def check(ok: bool, name: str, detail: str = "") -> bool:
    _results.append((ok, name, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f" — {detail}" if detail else ""))
    return ok


def _req(url: str, method: str = "GET", body: dict | None = None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"User-Agent": "yeobaek-smoke"}
    if data:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        raw = r.read().decode("utf-8", "replace")
        return r.status, (json.loads(raw) if raw.strip() else None)


def get(base: str, path: str, **params):
    url = base + path + ("?" + urllib.parse.urlencode(params) if params else "")
    return _req(url)


def post(base: str, path: str, body: dict):
    return _req(base + path, "POST", body)


def rpc(base: str, method: str, params: dict | None = None, _id: int = 1):
    msg = {"jsonrpc": "2.0", "id": _id, "method": method}
    if params is not None:
        msg["params"] = params
    return post(base, "/mcp", msg)


def main() -> int:
    base = (sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8000").rstrip("/")
    print(f"\n=== 여백 배포 스모크 테스트 ===\n대상: {base}\n")

    # ── 1. 서버 기동 · 데이터 적재 ────────────────────────────────
    print("[1/4] 서버 상태")
    try:
        st, h = get(base, "/health")
    except Exception as e:
        check(False, "/health 응답", f"{type(e).__name__}: {e}")
        print("\n서버에 연결하지 못했습니다. 주소·배포 상태를 먼저 확인하세요.")
        return 1
    check(st == 200 and (h or {}).get("status") == "ok", "/health 200 ok", f"status={st}")

    loaded = (h or {}).get("places_loaded", 0)
    check(loaded > 0, "장소 데이터 적재", f"places_loaded={loaded}"
          + ("" if loaded else "  ← DB가 비어 있습니다. collect_tourapi.py 후 재배포 필요"))
    print(f"    엔진: {'C++' if (h or {}).get('engine_available') else '파이썬 폴백'}"
          f" · 서울키 {'O' if (h or {}).get('seoul_api_key_set') else 'X'}"
          f" · 집중률키 {'O' if (h or {}).get('tats_api_key_set') else 'X'}"
          f" · 연관 {'O' if ((h or {}).get('related') or {}).get('configured') else 'X'}")
    if not (h or {}).get("seoul_api_key_set"):
        print("    ※ SEOUL_API_KEY 미설정 → 혼잡도가 전부 중립(보통)으로 나옵니다.")

    # ── 2. 앱 API ────────────────────────────────────────────────
    print("\n[2/4] 앱 API")
    sample = None
    try:
        st, s = get(base, "/api/v1/places/search", q="공원", limit=5)
        rows = (s or {}).get("results") or []
        if not rows:                       # 지역 편중 대비 2차 시도
            st, s = get(base, "/api/v1/places/search", q="관", limit=5)
            rows = (s or {}).get("results") or []
        check(st == 200, "/places/search 200", f"{len(rows)}건")
        sample = rows[0] if rows else None
    except Exception as e:
        check(False, "/places/search", f"{type(e).__name__}: {e}")

    if sample:
        print(f"    테스트 기준 장소: {sample['title']} (id={sample['content_id']})")
        try:
            st, m = post(base, "/api/v1/match",
                         {"content_id": sample["content_id"], "radius_km": 5, "top_k": 3})
            check(st == 200, "/match 200", f"twins={len((m or {}).get('twins') or [])}")
        except Exception as e:
            check(False, "/match", f"{type(e).__name__}: {e}")

        start = datetime.now(KST).strftime("%Y-%m-%dT%H:%M:00")
        try:
            st, p = post(base, "/api/v1/schedule",
                         {"start_time": start, "stops": [sample["content_id"]]})
            check(st == 200 and (p or {}).get("ordered"), "/schedule 200",
                  f"mode={(p or {}).get('scheduler_mode')}")
        except Exception as e:
            check(False, "/schedule", f"{type(e).__name__}: {e}")

        if sample.get("lat") and sample.get("lng"):
            try:
                st, hm = get(base, "/api/v1/places/heatmap",
                             lat=sample["lat"], lng=sample["lng"], radius_km=3)
                check(st == 200, "/places/heatmap 200",
                      f"{len((hm or {}).get('results') or [])}건")
            except Exception as e:
                check(False, "/places/heatmap", f"{type(e).__name__}: {e}")

    # ── 3. MCP 프로토콜 ──────────────────────────────────────────
    print("\n[3/4] MCP 서버 (POST /mcp)")
    tool_names: list[str] = []
    try:
        st, j = rpc(base, "initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "yeobaek-smoke", "version": "1"}})
        info = ((j or {}).get("result") or {}).get("serverInfo") or {}
        check(st == 200 and info.get("name") == "yeobaek", "initialize",
              f"{info.get('name')} v{info.get('version')}")
    except Exception as e:
        check(False, "initialize", f"{type(e).__name__}: {e}")

    try:
        st, j = rpc(base, "tools/list", {}, 2)
        tools = ((j or {}).get("result") or {}).get("tools") or []
        tool_names = [t["name"] for t in tools]
        check(st == 200 and len(tools) >= 5, "tools/list", f"{len(tools)}개: {', '.join(tool_names)}")
    except Exception as e:
        check(False, "tools/list", f"{type(e).__name__}: {e}")

    # ── 4. MCP 도구 실제 호출 ────────────────────────────────────
    print("\n[4/4] MCP 도구 호출")
    place_name = sample["title"] if sample else "경복궁"
    calls = [
        ("search_places", {"query": place_name[:3]}),
        ("get_congestion", {"place_name": place_name}),
        ("get_offpeak_hours", {"place_name": place_name}),
        ("find_quiet_alternative", {"place_name": place_name}),
        ("plan_quiet_course", {"place_names": [place_name]}),
    ]
    for i, (name, args) in enumerate(calls, start=10):
        if tool_names and name not in tool_names:
            check(False, f"tools/call {name}", "서버가 이 도구를 제공하지 않음")
            continue
        try:
            st, j = rpc(base, "tools/call", {"name": name, "arguments": args}, i)
            res = (j or {}).get("result") or {}
            text = ((res.get("content") or [{}])[0]).get("text", "")
            # 도구가 '못 찾음'을 안내하는 것도 정상 동작이다 — 프로토콜 실패만 FAIL.
            ok = st == 200 and bool(text) and "error" not in (j or {})
            check(ok, f"tools/call {name}", text.splitlines()[0][:70] if text else "빈 응답")
        except Exception as e:
            check(False, f"tools/call {name}", f"{type(e).__name__}: {e}")

    # ── 요약 ─────────────────────────────────────────────────────
    n_fail = sum(1 for ok, _, _ in _results if not ok)
    print("\n" + "=" * 62)
    print(f"결과: {len(_results) - n_fail}/{len(_results)} 통과"
          + (f", {n_fail} 실패" if n_fail else " — 전부 통과 ✅"))
    print("=" * 62)
    if not n_fail:
        print("\nAI 클라이언트 연결 설정:")
        print(json.dumps({"mcpServers": {"yeobaek": {"type": "http", "url": f"{base}/mcp"}}},
                         ensure_ascii=False, indent=2))
    return 1 if n_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
