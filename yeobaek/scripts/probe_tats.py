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

    import datetime
    today = datetime.date.today()
    recent = [(today - datetime.timedelta(days=d)).strftime("%Y%m%d") for d in (1, 7, 14, 30)]

    base = {
        "serviceKey": KEY,
        "MobileOS": "ETC",
        "MobileApp": "yeobaek",
        "numOfRows": "5",
        "pageNo": "1",
        "_type": "json",
    }

    # areaCd=11(서울), signguCd=11680(강남구) 로 필수값 채워 순차 시도.
    attempts = [
        ("areaCd=11", {**base, "areaCd": "11"}),
        ("areaCd=11 & signguCd=11680", {**base, "areaCd": "11", "signguCd": "11680"}),
    ]
    for ymd in recent:
        attempts.append((f"areaCd=11 & signguCd=11680 & baseYmd={ymd}",
                         {**base, "areaCd": "11", "signguCd": "11680", "baseYmd": ymd}))

    def show_fields(body: str) -> bool:
        try:
            j = json.loads(body)
        except Exception:
            return False
        rc = j.get("response", {}).get("header", {}).get("resultCode")
        items = j.get("response", {}).get("body", {}).get("items", {})
        item = items.get("item") if isinstance(items, dict) else None
        first = item[0] if isinstance(item, list) and item else item
        if isinstance(first, dict):
            print("  --- 첫 항목 필드명 ---")
            for k, v in first.items():
                print(f"    {k} = {v}")
            return True
        return rc == "0000"

    for label, params in attempts:
        print(f"\n=== {label} ===")
        try:
            body = call(params)
            print(body[:2500])
            if show_fields(body) and "item" in body:
                print("\n>>> 성공! 위 필드명을 붙여주세요.")
                break
        except Exception as e:
            print(f"오류: {e}")

    print("\n※ 여전히 에러면 그 resultMsg 를 알려주세요(다음 필수 파라미터를 알 수 있음).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
