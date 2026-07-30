"""전국 관광지 집중률 예측 API(TatsCnctrRateService) 응답 샘플 확인.

한국관광공사_관광지 집중률 방문자 추이 예측 정보 → tatsCnctrRatedList.
응답 필드명을 확정해 연동 클라이언트를 붙이기 위한 1회성 probe (stdlib 만 사용).

사용:
    export TATS_API_KEY=발급받은_일반인증키(Decoding)
    cd yeobaek && python scripts/probe_tats.py
"""
from __future__ import annotations
import json
import os
import sys
import urllib.parse
import urllib.request

ENDPOINT = os.environ.get(
    "TATS_ENDPOINT",
    "https://apis.data.go.kr/B551011/TatsCnctrRateService/tatsCnctrRatedList")
KEY = os.environ.get("TATS_API_KEY", "")


def call(params: dict) -> str:
    qs = urllib.parse.urlencode(params, safe="")
    url = f"{ENDPOINT}?{qs}"
    req = urllib.request.Request(url, headers={"User-Agent": "yeobaek-probe"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read().decode("utf-8", "replace")


def main() -> int:
    if not KEY:
        print("ERROR: TATS_API_KEY 환경변수가 필요합니다.", file=sys.stderr)
        return 2

    base = {
        "serviceKey": KEY,
        "MobileOS": "ETC",
        "MobileApp": "yeobaek",
        "numOfRows": "5",
        "pageNo": "1",
        "_type": "json",
    }

    # 기본 호출부터. 필수 파라미터가 더 필요하면 에러 메시지로 드러난다.
    print("=== [1] 기본 호출 (numOfRows=5, _type=json) ===")
    try:
        body = call(base)
        print(body[:4000])
        # JSON 이면 첫 item 의 키를 뽑아 보여준다(필드명 확인용)
        try:
            j = json.loads(body)
            items = (j.get("response", {}).get("body", {})
                       .get("items", {}))
            item = items.get("item") if isinstance(items, dict) else None
            first = item[0] if isinstance(item, list) and item else item
            if isinstance(first, dict):
                print("\n--- 첫 항목 필드명 ---")
                for k, v in first.items():
                    print(f"  {k} = {v}")
        except Exception:
            pass
    except Exception as e:
        print(f"[1] 오류: {e}")

    # 참고: 지역/기준일 필터가 필요할 수 있어 후보 파라미터도 안내
    print("\n※ 위가 에러(필수 파라미터 누락 등)면, 매뉴얼의 필수 항목"
          "(예: signguCode, baseYmd=YYYYMMDD, areaCd 등)을 알려주세요.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
