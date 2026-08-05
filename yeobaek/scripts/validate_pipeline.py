"""오프라인 파이프라인 검증 하네스 — 키/외부망 없이 데이터 파이프라인 전체 배관을 실행·검증한다.

배경(중요):
    실서비스 데이터 완주(collect_tourapi → build_area_map → build_embeddings)에는
      1) TourAPI/서울시 API 키,  2) data.go.kr·huggingface.co 로의 외부 egress
    두 가지가 모두 필요하다. 샌드박스/CI 에는 이 둘이 없어(egress 정책 차단),
    collect_tourapi.py 와 build_embeddings.py 의 '실측 fetch' 는 이 환경에서 실행 불가다.

    이 하네스는 키·외부망 없이도 검증 가능한 나머지 전 구간을 '실제 프로덕션 코드'로 돌린다:
      • places 테이블 적재 (실측 서울 명소 fixture — 좌표는 진짜)
      • build_area_map.py  → 실제 C++ 엔진으로 서울 121 예보지점 매핑 (커버리지 = 실측)
      • EmbeddingStore / find_twins / 전 엔드포인트 (/health /match /schedule /card /places/search)

    임베딩 벡터만은 e5-small(huggingface) 다운로드가 막혀 '구조적 스탠드인'을 쓴다.
    → 코사인 랭킹·엔드포인트 '배관' 은 검증되지만, e5 의 '의미적 품질'(경복궁≈덕수궁 여부)은
       검증 대상이 아니다. 그 항목은 키·HF 접근이 되는 개발망에서 build_embeddings.py 로 확인한다.

사용:  cd yeobaek && python scripts/validate_pipeline.py
반환:  전 항목 통과 시 0, 하나라도 실패 시 1.
"""
from __future__ import annotations
import hashlib
import json
import os
import sqlite3
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "seoul_places.json"
EMB_DIM = 384  # intfloat/multilingual-e5-small 차원 — 스탠드인도 동일 차원으로 맞춘다

# ── 격리된 임시 작업공간을 프로덕션 코드가 보기 전에 env 로 주입한다 ──
_WORK = Path(tempfile.mkdtemp(prefix="yeobaek_validate_"))
os.environ["YEOBAEK_DB"] = str(_WORK / "yeobaek.db")
os.environ["YEOBAEK_EMB"] = str(_WORK / "embeddings.npy")
os.environ["YEOBAEK_EMB_IDS"] = str(_WORK / "embeddings_ids.npy")
os.environ.setdefault("SEOUL_API_KEY", "")  # 키 없이도 좌표 매핑은 동작

sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "engine" / "build"))

from server.config import settings          # noqa: E402  (env 주입 이후 import)
from server.db import repository            # noqa: E402

PASS, FAIL = "PASS", "FAIL"
_results: list[tuple[str, str, str]] = []


def check(name: str, ok: bool, detail: str = "") -> bool:
    _results.append((PASS if ok else FAIL, name, detail))
    print(f"  [{PASS if ok else FAIL}] {name}" + (f" — {detail}" if detail else ""))
    return ok


def load_fixture() -> list[dict]:
    return json.loads(FIXTURE.read_text(encoding="utf-8"))["places"]


def seed_places(places: list[dict]) -> None:
    repository.init_db()
    con = sqlite3.connect(settings.DB_PATH)
    con.executemany(
        "INSERT INTO places(content_id,title,addr,mapx,mapy,area_code,sigungu_code,cat,overview,updated_at)"
        " VALUES(:content_id,:title,NULL,:mapx,:mapy,1,NULL,:cat,:overview,datetime('now'))"
        " ON CONFLICT(content_id) DO UPDATE SET title=excluded.title, mapx=excluded.mapx,"
        " mapy=excluded.mapy, cat=excluded.cat, overview=excluded.overview",
        places,
    )
    con.commit()
    con.close()


def build_structural_embeddings(places: list[dict]) -> None:
    """구조적 임베딩 스탠드인(‼️ e5 아님). 같은 cluster 는 코사인↑, 다른 cluster 는 코사인↓
    가 되도록 결정적으로 생성한다. EmbeddingStore/find_twins '배관'과 랭킹 로직 검증 전용.
    실측 의미 임베딩은 build_embeddings.py(e5-small)로 개발망에서 생성한다."""
    clusters = sorted({p["cluster"] for p in places})
    cidx = {c: i for i, c in enumerate(clusters)}
    ids = np.array([p["content_id"] for p in places], dtype=np.int64)
    vecs = np.zeros((len(places), EMB_DIM), dtype=np.float32)
    for i, p in enumerate(places):
        # 지배적 cluster 축(강하게) + content_id 해시 기반 결정적 잡음(약하게)
        vecs[i, cidx[p["cluster"]] % EMB_DIM] = 3.0
        h = hashlib.sha256(str(p["content_id"]).encode()).digest()
        noise = np.frombuffer(h * (EMB_DIM // len(h) + 1), dtype=np.uint8)[:EMB_DIM]
        vecs[i] += (noise.astype(np.float32) / 255.0 - 0.5) * 0.2
    norms = np.linalg.norm(vecs, axis=1, keepdims=True)
    vecs /= norms
    np.save(settings.EMB_VECTORS, vecs)
    np.save(settings.EMB_IDS, ids)


def main() -> int:
    places = load_fixture()
    print(f"\n=== 여백 오프라인 파이프라인 검증 ===\n작업공간: {_WORK}")
    print(f"fixture: 실측 서울 명소 {len(places)}건\n")

    # ── 1) places 적재 ──────────────────────────────────────────────
    print("[1/5] places 테이블 적재")
    seed_places(places)
    loaded = repository.all_places()
    check("places 적재(좌표 보유)", len(loaded) == len(places), f"{len(loaded)}/{len(places)}건")

    # ── 2) build_area_map.py (실제 프로덕션 스크립트 + 실 C++ 엔진) ────
    print("\n[2/5] build_area_map.py — 실 엔진으로 서울 예보지점 매핑")
    import importlib
    bam = importlib.import_module("scripts.build_area_map")
    rc = bam.main()
    check("build_area_map 실행", rc == 0, f"rc={rc}")
    con = sqlite3.connect(settings.DB_PATH)
    mapped_rows = con.execute(
        "SELECT p.title, m.seoul_area_name, m.dist_km FROM place_area_map m "
        "JOIN places p ON p.content_id=m.content_id ORDER BY m.dist_km"
    ).fetchall()
    con.close()
    n_mapped = len(mapped_rows)
    coverage = 100.0 * n_mapped / len(places)
    check("예보지점 매핑 발생", n_mapped > 0, f"{n_mapped}/{len(places)}건 매핑, 커버리지 {coverage:.0f}%")
    print("    ── 실측 매핑 결과 (검증 질문 Q2: 예보지점 커버리지) ──")
    for title, area, dist in mapped_rows:
        print(f"      {title:<12} → {area:<10} ({dist:.2f} km)")
    # 매핑 안 된 장소(중립 폴백) 목록
    con = sqlite3.connect(settings.DB_PATH)
    mapped_titles = {r[0] for r in con.execute(
        "SELECT p.title FROM place_area_map m JOIN places p ON p.content_id=m.content_id"
    ).fetchall()}
    con.close()
    unmapped = [p["title"] for p in places if p["title"] not in mapped_titles]
    if unmapped:
        print(f"    ── 서울 예보권 밖(>4km) → 중립 폴백: {', '.join(unmapped)}")

    # ── 3) 임베딩(구조적 스탠드인) + 로드 ─────────────────────────────
    print("\n[3/5] 임베딩 스탠드인 생성 + EmbeddingStore 로드")
    print("    ⚠️  e5-small(huggingface) egress 차단 → 구조적 스탠드인 사용")
    print("        (코사인 랭킹 '배관' 검증 전용, 의미 품질은 개발망 build_embeddings.py 로 확인)")
    build_structural_embeddings(places)
    from server.services.embedding import EmbeddingStore
    store = EmbeddingStore(settings.EMB_VECTORS, settings.EMB_IDS)
    check("EmbeddingStore 로드", store.ok and store.dim == EMB_DIM, f"dim={store.dim}, n={len(places)}")

    # ── 4) 엔진 로드 + find_twins (반경∩코사인∩예보지점 결합) ──────────
    print("\n[4/5] 엔진 로드 + find_twins 배관")
    from server.engine import engine_state
    engine_state.init()
    check("C++ 엔진 available", engine_state.available, f"places_loaded={engine_state.places_loaded}")
    from server.services.match_service import find_twins
    gbg = repository.get_place(900001)  # 경복궁
    twins = find_twins(gbg, radius_km=5.0, top_k=3)
    check("경복궁 감성 쌍둥이 top-3 반환", len(twins) > 0,
          "쌍둥이: " + ", ".join(f"{t['title']}({t['similarity']:.2f})" for t in twins))
    # 구조적 스탠드인 특성상 같은 cluster(궁궐)가 상위여야 랭킹 배관이 정상
    palace_ids = {900002, 900003, 900004}
    top_is_palace = twins and twins[0]["content_id"] in palace_ids
    check("랭킹 배관 정상(top-1 이 동일 cluster)", bool(top_is_palace),
          f"top-1 = {twins[0]['title'] if twins else 'N/A'}")

    # ── 5) FastAPI 엔드포인트 E2E (TestClient) ───────────────────────
    print("\n[5/5] FastAPI 엔드포인트 E2E")
    from fastapi.testclient import TestClient
    from server.main import app
    with TestClient(app) as client:
        h = client.get("/health").json()
        check("/health engine_available", h.get("engine_available") is True, json.dumps(h, ensure_ascii=False))

        m = client.post("/api/v1/match", json={"content_id": 900001, "radius_km": 5, "top_k": 3})
        mj = m.json()
        check("/api/v1/match 200 + twins", m.status_code == 200 and len(mj.get("twins", [])) > 0,
              f"status={m.status_code}, twins={len(mj.get('twins', []))}")

        s = client.post("/api/v1/schedule", json={
            "start_time": "2026-10-11T10:00:00",
            "stops": [900001, 900016, 900008],   # 경복궁, 국립중앙박물관, 명동거리
            "allow_substitution": True,
        })
        sj = s.json()
        check("/api/v1/schedule 200 + ordered", s.status_code == 200 and "ordered" in sj,
              f"status={s.status_code}, stops={len(sj.get('ordered', []))}")

        # 경복궁(source) → 창덕궁(alt) 설득 카드
        c = client.post("/api/v1/card", json={"source_id": 900001, "alt_id": 900002})
        check("/api/v1/card 200", c.status_code == 200, f"status={c.status_code}")

        p = client.get("/api/v1/places/search", params={"q": "궁"})
        check("/api/v1/places/search 200", p.status_code == 200, f"status={p.status_code}")

    # ── 요약 ─────────────────────────────────────────────────────────
    n_fail = sum(1 for r in _results if r[0] == FAIL)
    print("\n" + "=" * 60)
    print(f"검증 요약: {len(_results) - n_fail}/{len(_results)} 통과"
          + (f", {n_fail} 실패" if n_fail else " — 전부 통과 ✅"))
    print(f"실측 예보지점 커버리지(fixture {len(places)}건 기준): {coverage:.0f}% ({n_mapped}/{len(places)})")
    print("=" * 60)
    print("\n남은 실서비스 완주(이 환경 밖, 키+개발망 필요):")
    print("  export TOURAPI_KEY=... SEOUL_API_KEY=...")
    print("  python scripts/collect_tourapi.py --pages 5   # data.go.kr egress 필요")
    print("  python scripts/build_area_map.py              # ← 여기 배관은 방금 검증됨")
    print("  python scripts/build_embeddings.py            # huggingface egress 필요")
    return 1 if n_fail else 0


if __name__ == "__main__":
    raise SystemExit(main())
