# 여백(Yeobaek)

> "같은 감성, 다른 시간/장소" — 도착 시점 예측 혼잡도가 낮고 감성적으로 유사한
> 대안지를 추천하는 **시간축 재배열 코스 플래너**.

CrowdMap 자산(C++ 서울 도시데이터 클라이언트·공간 해시·캐시)을 이식·확장해,
`홈 → 플래너 → 대안카드`가 실제 데이터로 도는 End-to-End MVP 를 구성한다.
이 디렉터리는 상위 `crowdmap` 레포와 충돌하지 않도록 `yeobaek/` 아래에 자립적으로 둔다.

---

## 구조

```
yeobaek/
├─ engine/                     # C++17 엔진 (pybind11 → yeobaek_engine)
│  ├─ external/                #  ExternalCongestionClient(+getForecast, 결정 A)
│  │                           #  SeoulCityDataClient(이식), SeoulCityDataForecastClient(신규, FCST_PPLTN)
│  ├─ congestion/ForecastProvider.h   # (area,timeBucket) 캐시 + fallback 체인 (결정 B)
│  ├─ spatial/                 #  SpatialHash(이식) + SpatialIndex(반경 쿼리)
│  ├─ scheduler/Scheduler.h    #  시간의존 TSP (완전탐색/2-opt, 부록 C)
│  ├─ bindings/pybind_module.cpp
│  ├─ tests/scheduler_test.cpp # 오프라인 결정적 검증
│  └─ CMakeLists.txt
├─ server/                     # FastAPI
│  ├─ api/ (match, schedule, card)
│  ├─ services/ (embedding, tourapi, rag, match_service)
│  ├─ db/ (schema.sql, repository.py)
│  ├─ engine.py                # yeobaek_engine 로드 + SpatialIndex 적재
│  └─ main.py                  # 앱 + /health
├─ scripts/ (collect_tourapi, build_area_map, build_embeddings)
└─ data/ (yeobaek.db, embeddings.npy — 스크립트로 생성, gitignore)
```

---

## 빌드 & 실행

### 1) C++ 엔진 빌드 (pybind11)
```bash
# 의존: cmake, g++(C++17), libcurl, nlohmann-json, pybind11(pip 설치 가능)
pip install pybind11 numpy
cd yeobaek/engine && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
# → yeobaek_engine.*.so 가 server/ 옆으로 복사됨
```

### 2) 데이터 파이프라인 (Phase 1)
```bash
cd yeobaek
pip install -r scripts/requirements.txt
export TOURAPI_KEY=발급키                # 한국관광공사(필수 활용)
python scripts/collect_tourapi.py --pages 5   # places 적재(overview 없는 항목 스킵)
python scripts/build_area_map.py              # 장소 → 서울 121 예보지점 매핑
python scripts/build_embeddings.py            # e5-small 임베딩(L2 정규화)
```

### 3) 서버 실행
```bash
cd yeobaek
pip install -r server/requirements.txt
export SEOUL_API_KEY=서울열린데이터광장키     # FCST_PPLTN 예보(모듈2)
# (선택) export YEOBAEK_USE_LLM=1 ANTHROPIC_API_KEY=...   # RAG 재서술(옵션)
uvicorn server.main:app --reload
# GET /health, POST /api/v1/{match,schedule,card}
```

> `SEOUL_API_KEY` 가 없으면 예보는 **네트워크 호출 없이 중립값(보통)** 으로 폴백한다
> (오프라인/데모 안전). 매칭·스케줄러·카드는 그대로 동작한다.

### 4) Android 앱 (홈→플래너→대안카드, Phase 4)
CrowdMap 앱 모듈(`app/`)에 여백 화면들을 **별도 런처("여백")** 로 추가했다(기존 CrowdMap 런처 무영향).
- **디자인**(승인 시안, 네이버지도×테이블링): `Theme.Yeobaek` — 브랜드 그린 `#0FB86B`,
  페이퍼 화이트 `#F7F6F2`, 혼잡 시맨틱 히트 스케일(여유#2E9E5B→붐빔#DB4437).
- **홈 = 풀스크린 지도**: 구글맵(`ye_map_style.json` 그린 톤) + 떠 있는 검색바 + 하단 시트.
  검색으로 담은 장소가 **그린 마커**로 지도에 바로 표시되고 카메라가 자동으로 맞춰진다.
  - 구글맵은 기존 CrowdMap 이 쓰던 `com.google.android.geo.API_KEY` 를 그대로 재사용(새 키 불필요).
- **지역 추천**: 지도를 옮기면(카메라 idle) 보이는 지역의 명소를 `/places/nearby` 로 받아
  상단에 **"이 지역 추천 · 플래너에 넣을까요?"** 가로 카드로 띄우고, `＋ 담기` 로 바로 방문지에 추가.
- **코스 방식 2모드**(하단 시트 토글):
  - `자동 최적화` — 혼잡도를 예측해 방문 순서를 재배치(`/schedule` 기본).
  - `내 순서대로` — 내가 고른 순서를 유지하고 도착 시점 혼잡도만 채운다(`keep_order=true`).
- 흐름: `홈` → `SearchActivity`(`/places/search`, 좌표 포함) → `/schedule`
  → `Planner`(타임라인 카드·혼잡 배지, 모드별 헤더, 고혼잡 "대안 보기" → `/match`) → `Alternatives`(감성 쌍둥이)
  → `Card`(`/card` 근거 + **원탭 스왑 → 재스케줄**)
- 네트워킹: Retrofit2 + Gson + Coroutines(`lifecycleScope`), 디바운스 검색, 로딩/에러 처리.
- 실행: Android Studio 로 열고(Gradle sync) 실기기/에뮬레이터에서 "여백" 아이콘 실행.
  - `local.properties` 에 서버 IP: `SERVER_IP=192.168.x.x` (PC의 LAN IP). 포트는 `8000` 고정(`YeobaekClient.PORT`).
  - 서버는 폰에서 접근되게 바인딩: `uvicorn server.main:app --host 0.0.0.0 --port 8000`
  - 에뮬레이터면 호스트 PC = `10.0.2.2` → `SERVER_IP=10.0.2.2`
  - 로컬 평문 HTTP 허용을 위해 `usesCleartextTraffic=true` 설정됨(개발용).

> ⚠️ Android 코드는 이 저장소 환경(Android SDK/Gradle 부재)에서 **빌드 검증은 못 했다** —
> Android Studio 에서 최초 빌드가 필요하다. 엔진/서버는 실행까지 검증됨.

---

## API (부록 B)

- `POST /api/v1/match` — `content_id` → 반경 후보(C++) → numpy 코사인 → 감성 쌍둥이 top-K(+예보 혼잡도)
- `POST /api/v1/schedule` — `start_time`+`stops`+`weights`(+`keep_order`) → 코스.
  `keep_order=false`(기본)=시간의존 최적 재배치, `true`=고른 순서 유지(도착시점 예보만).
- `POST /api/v1/card` — `source_id`,`alt_id` → 설득 카드. **수치는 코드가 계산, LLM 은 문장만(결정 G)**
- `GET  /api/v1/places/search?q=` — 이름 부분일치 검색(+cat_label, **lat/lng**). 앱 홈 지도 마커용.
- `GET  /api/v1/places/nearby?lat=&lng=&radius_km=` — 좌표 반경 내 명소를 가까운 순으로.
  앱에서 **지도를 옮기면 "이 지역 추천 · 플래너에 넣을까요?"** 카드에 사용.

---

## 아키텍처 결정 (요약)

| | 내용 |
| --- | --- |
| A | 혼잡도 인터페이스에 시간축 확장 — base 에 `getForecast()` default(invalid) 추가 |
| B | 예측 경로/실시간 경로 분리 — 신규 `ForecastProvider`((area,timeBucket) 캐시) |
| C | 예보 스코프 = 서울 121장소 고정, 장소↔예보지점 사전 매핑 |
| D | 스케줄러 = 시간의존 소규모 TSP (완전탐색 ≤8, 이상 2-opt) |
| E | 매칭 = C++ 반경필터 + Python numpy 코사인(e5) 분리 |
| G | RAG 환각 차단 — 템플릿 기본, LLM 재서술 옵션, 숫자는 코드가 계산 |

fallback 체인(예보 부재): self 예보 → 인근 장소 예보 → 실시간 `AREA_CONGEST_LVL` → 중립값.

## 스케줄러 비용함수 (부록 C)
```
cost(order) = w_c·Σ c_i + w_d·(총이동거리/D_max) + w_s·Σ(1−sim_j)·[치환]
             c_i = level_i/4,  기본 w_c=1.0 w_d=0.3 w_s=0.5
```
치환은 **원본 stop 의 도착시점 예보가 고혼잡 임계(기본 3) 이상일 때만** 고려하며,
같은 장소 중복 방문을 막는다(저혼잡에서는 원본 유지).

---

## 해시태그 ↔ 기능 매핑 (Phase 5-3 기능설명서)

| 해시태그 | 구현 지점 |
| --- | --- |
| `#혼잡도분석` | 모듈2 `SeoulCityDataForecastClient` — `FCST_PPLTN` 도착시점 예보 파싱 |
| `#과밀지역우회` | `Scheduler` 고혼잡 감지 → 감성 쌍둥이 스왑(부록 C) |
| `#숨은명소` | 모듈1 e5 임베딩 유사도 + 근거 RAG 카드 |
| `#코스추천` | 시간의존 스케줄러 완결 일정(`/schedule`) |

OpenAPI 활용: **TourAPI(한국관광공사)** = 소개글/좌표/카테고리 수집(필수),
**서울 실시간 도시데이터** = 도착시점 예보(외부 추가 API).

---

## 검증(이 저장소에서 실행 확인)

- `engine/tests/scheduler_test.cpp` — 시간의존 재정렬·치환·유사도 페널티 결정적 검증
- FastAPI `TestClient` E2E — `/health`·`/match`·`/schedule`·`/card` 실제 응답(seed DB)

## 로드맵 (미구현/확장)
- Phase 4 Android 3화면(Retrofit) + 모듈4 `TrackingCore` 실시간 급증 스왑 — `router.resolve_now` 노출 완료, 앱 연동은 확장 단계
- 전국 예보 확장(한국관광 데이터랩 혼잡도), C++ 코사인 최적화, 실측 라우팅 API
