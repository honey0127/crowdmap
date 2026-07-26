"""Phase 0 실측 도구 — 서울 FCST_PPLTN + TourAPI 스키마를 한 번에 확인한다.

**표준 라이브러리만 사용**(requests/pip 불필요). 최소 파이썬(MSYS2 등)에서도 동작.

키는 환경변수에서만 읽는다(파일에 저장 금지):
  PowerShell:  $env:SEOUL_API_KEY="..."; $env:TOURAPI_KEY="..."; python scripts/probe_apis.py
  bash:        SEOUL_API_KEY=... TOURAPI_KEY=... python scripts/probe_apis.py [지역명]

출력의 "FCST_TIME 포맷 / 지평(horizon)"을 개발자에게 공유하면 파서를 맞출 수 있다.
※ 한국 공공 API 로 egress 가 열린 로컬/개발망에서 실행(일부 CI/샌드박스는 차단).
"""
from __future__ import annotations
import json
import os
import ssl
import sys
from urllib.parse import quote, urlencode
from urllib.request import urlopen
from urllib.error import URLError, HTTPError

TOUR_BASE = "https://apis.data.go.kr/B551011/KorService2"


def _get(url: str, timeout: int = 15) -> tuple[int, str]:
    """(status, body). HTTPS 인증서 검증 실패 시(최소 파이썬 CA 부재) 진단 목적
    비검증 컨텍스트로 1회 재시도한다(공개 데이터 조회 한정)."""
    try:
        with urlopen(url, timeout=timeout) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except URLError as e:
        if isinstance(e.reason, ssl.SSLError):
            print("   ! TLS 인증서 검증 실패 → 비검증 재시도(진단용, 공개데이터 한정)")
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            with urlopen(url, timeout=timeout, context=ctx) as r:
                return r.status, r.read().decode("utf-8", "replace")
        raise


def probe_seoul(key: str, area: str) -> None:
    print("=" * 64, f"\n[1] 서울 실시간 도시데이터 — {area} FCST_PPLTN\n" + "=" * 64)
    url = f"http://openapi.seoul.go.kr:8088/{key}/json/citydata_ppltn/1/1/{quote(area)}"
    try:
        status, body = _get(url, timeout=10)
    except Exception as e:
        print("네트워크 실패:", type(e).__name__, e)
        print("→ 이 호스트(:8088)로 egress 가 막힌 환경일 수 있습니다. 로컬에서 실행하세요.")
        return
    print("HTTP", status, "len", len(body))
    try:
        j = json.loads(body)
    except ValueError:
        print("비JSON 응답:", body[:300]); return
    root = j.get("SeoulRtd.citydata_ppltn")
    if not isinstance(root, list) or not root:
        print("citydata_ppltn 파싱 실패. 원문:", json.dumps(j, ensure_ascii=False)[:400]); return
    d = root[0]
    print("AREA_NM:", d.get("AREA_NM"), "| 실시간 AREA_CONGEST_LVL:", d.get("AREA_CONGEST_LVL"))
    fc = d.get("FCST_PPLTN")
    if not isinstance(fc, list) or not fc:
        print("FCST_PPLTN 없음/빈값:", repr(fc)[:200]); return
    print(f"FCST_PPLTN 개수: {len(fc)} | 항목 키: {list(fc[0].keys())}")
    for e in fc[:3] + (fc[-1:] if len(fc) > 3 else []):
        print("   FCST_TIME=", e.get("FCST_TIME"), "| LVL=", e.get("FCST_CONGEST_LVL"),
              "| MIN=", e.get("FCST_PPLTN_MIN"), "MAX=", e.get("FCST_PPLTN_MAX"))
    print(f"지평(horizon): {fc[0].get('FCST_TIME')}  ~  {fc[-1].get('FCST_TIME')}")
    print("※ FCST_TIME 포맷이 'YYYY-MM-DD HH:MM' 가 아니면 파서 수정 필요.")


def probe_tour(key: str) -> None:
    print("\n" + "=" * 64, "\n[2] TourAPI KorService2 — areaBasedList2/detailCommon2 (서울)\n" + "=" * 64)
    common = {"serviceKey": key, "MobileOS": "ETC", "MobileApp": "yeobaek", "_type": "json"}
    # KorService2: listYN 파라미터 없음(있으면 INVALID_REQUEST_PARAMETER_ERROR)
    q = urlencode({**common, "areaCode": 1, "numOfRows": 3, "pageNo": 1, "arrange": "C"})
    try:
        status, body = _get(f"{TOUR_BASE}/areaBasedList2?{q}", timeout=15)
    except Exception as e:
        print("네트워크 실패:", type(e).__name__, e)
        print("→ apis.data.go.kr 로 egress 가 막힌 환경일 수 있습니다. 로컬에서 실행하세요.")
        return
    print("HTTP", status)
    try:
        b = json.loads(body)["response"]["body"]
    except Exception:
        print("응답 파싱 실패(키·승인상태 확인). 원문:", body[:400]); return
    items = b.get("items")
    it = (items.get("item") if isinstance(items, dict) else items) or []
    if isinstance(it, dict):
        it = [it]
    print("totalCount:", b.get("totalCount"), "| 받은 개수:", len(it))
    if not it:
        return
    first = it[0]
    print("list 항목 키:", list(first.keys()))
    print("샘플:", {k: first.get(k) for k in
                  ("contentid", "title", "mapx", "mapy", "areacode", "sigungucode",
                   "cat1", "cat2", "cat3")})
    cid = first.get("contentid")
    # KorService2 detailCommon2: *YN 플래그 없이 contentId 만으로 기본 반환
    q2 = urlencode({**common, "contentId": cid})
    try:
        _, body2 = _get(f"{TOUR_BASE}/detailCommon2?{q2}", timeout=15)
        di = json.loads(body2)["response"]["body"]["items"]["item"]
        if isinstance(di, list):
            di = di[0]
    except Exception as e:
        print("detailCommon2 실패:", type(e).__name__, e); return
    ov = di.get("overview") or ""
    print(f"\ndetailCommon2 키: {list(di.keys())}")
    print(f"overview 길이: {len(ov)} | 앞부분: {ov[:120].replace(chr(10), ' ')}")


def main() -> int:
    area = sys.argv[1] if len(sys.argv) > 1 else "경복궁"
    sk, tk = os.environ.get("SEOUL_API_KEY"), os.environ.get("TOURAPI_KEY")
    if not sk and not tk:
        print("SEOUL_API_KEY / TOURAPI_KEY 환경변수를 설정하세요.", file=sys.stderr)
        return 2
    if sk:
        probe_seoul(sk, area)
    else:
        print("[1] SEOUL_API_KEY 미설정 — 건너뜀")
    if tk:
        probe_tour(tk)
    else:
        print("[2] TOURAPI_KEY 미설정 — 건너뜀")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
