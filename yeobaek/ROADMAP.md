# 여백(Yeobaek) — 진행 현황 · 다음 작업 · 개선 아이디어

> 이 문서는 **인수인계/현황 요약**용이다. 빌드·실행 방법과 아키텍처 상세는
> [`README.md`](./README.md), 원 절차서는 업로드한 Build Plan 을 참고.
> (기준: 아래 "완료"는 이 저장소에서 실제로 빌드·검증까지 마친 것만 표기)

---

## 0. 한눈에 보는 상태

| 영역 | 절차서 | 상태 | 비고 |
| --- | --- | --- | --- |
| C++ 엔진 이식·확장 | Phase 0, 2 | ✅ 완료·검증 | `.so` 빌드/임포트, 단위테스트 통과 |
| 예보 클라이언트(FCST_PPLTN) | 2-1 | 🟡 코드 완료 | **실 API 키로 스키마·지평 실측 미완(가장 큰 리스크)** |
| ForecastProvider 캐시 | 2-2 | ✅ 완료 | fallback 체인 포함 |
| Scheduler(시간의존 TSP) | 2-3 | ✅ 완료·검증 | 재정렬·치환·중복방지 단위테스트 |
| pybind11 바인딩 | 2-4 | ✅ 완료 | GIL 해제, keep_alive |
| FastAPI 3 엔드포인트 | Phase 3 | ✅ 완료·검증 | TestClient E2E |
| RAG 카드(템플릿+LLM) | 3-3 | 🟡 코드 완료 | 템플릿 검증 완료, LLM 경로는 키 필요 |
| 데이터 스크립트 | Phase 1 | 🟡 코드 완료 | `build_area_map`만 실행 검증(키 불필요) |
| Android 3화면 | Phase 4 | ⬜ 미착수 | 범위 제외 |
| 모듈4 실시간 스왑 | 4-5 | 🟡 훅만 | `router.resolve_now` 노출, 앱 연동 미완 |
| 통합·안정화·기능설명서 | Phase 5 | 🟡 일부 | 해시태그 매핑/README 착수, valgrind·시연 고정 미완 |

✅ 완료·검증  🟡 코드는 있으나 실데이터/실행 검증 남음  ⬜ 미착수

---

## 1. 내가 완료한 것 (검증 방식 포함)

### C++ 엔진 `engine/`
- **결정 A**: `ExternalCongestionClient`에 `getForecast()` 시간축 확장(base default = invalid) — 기존 실시간 클라이언트 무변경.
- **`SeoulCityDataForecastClient`**: `FCST_PPLTN`에서 도착시각에 가장 가까운 `FCST_TIME` 선택, 예보 부재 시 같은 payload 실시간값 폴백.
- **`ForecastProvider`**: `(area, timeBucket)` 캐시(성공 길게/폴백 짧게 TTL), 예보 부재 시 **self 예보 → 인근 장소 예보 → 실시간 → 중립값** 체인.
- **`SpatialIndex`**: 그리드 버킷 + haversine 반경 쿼리(모듈1 후보 추림).
- **`Scheduler`**: 완전탐색(≤8)/2-opt, 비용함수(부록 C), **고혼잡 임계(기본 3) 이상일 때만 감성 쌍둥이 치환 + 중복 방문 방지**.
- **안전장치**: API 키 미설정 시 네트워크를 아예 시도하지 않고 중립값 폴백(오프라인/데모 안전).
- **검증**: `engine/tests/scheduler_test.cpp` — 시간의존 재정렬(경복궁을 뒤로 미뤄 L4→L1, 43% 절약)·치환·유사도 페널티를 결정적으로 확인.

### FastAPI 서버 `server/`
- `/api/v1/match` · `/schedule` · `/card`(부록 B 계약) + `/health`.
- 매칭: C++ 반경필터 + numpy 코사인 top-K(결정 E).
- 카드: **수치는 코드가 계산, LLM은 문장만**(결정 G) — 템플릿 기본, `YEOBAEK_USE_LLM=1`이면 Anthropic SDK로 재서술(어떤 실패든 템플릿 폴백).
- SQLite repository/schema(부록 A), 부팅 시 SpatialIndex 적재.
- **검증**: FastAPI `TestClient` E2E — seed DB 로 4개 응답 확인(경복궁→덕수궁/운현궁/창덕궁 매칭, 3-stop 스케줄, 카드 grounded_facts).

### 데이터 파이프라인 `scripts/`
- `collect_tourapi.py`(서울 관광지 + overview 수집), `build_area_map.py`(예보지점 매핑), `build_embeddings.py`(e5-small, L2 정규화).
- **검증**: `build_area_map.py`는 실제로 실행해 5개 장소를 서울 121지점에 매핑 확인(키 불필요).

---

## 2. 다음에 해야 할 일 (우선순위 순)

### 🔴 P0 — 컨셉 게이트: 예보 실측 (절차서 Phase 0-1)
> 여기서 예보가 기대대로 안 나오면 시간축 설계 자체를 조정해야 하므로 **가장 먼저**.
- [ ] `SEOUL_API_KEY`로 `citydata_ppltn` 실호출 → `FCST_PPLTN[]`의 `FCST_TIME/FCST_CONGEST_LVL/FCST_PPLTN_MIN/MAX` 존재·구조 확인.
- [ ] **예보 지평(horizon)** 확인: 몇 시간 앞까지, 몇 분/시간 간격인지 → 데모 일정 범위 결정.
- [ ] `FCST_TIME` 실제 포맷 검증 — 현재 코드는 `"YYYY-MM-DD HH:MM"`(KST)로 가정. 다르면 `parseKstToEpoch` 수정.
- [ ] TourAPI 실측(0-2): `mapx=경도 / mapy=위도` 규약, `overview` 길이·품질 샘플 점검.

### 🟠 P1 — 실데이터로 파이프라인 1회 완주 (Phase 1)
- [ ] `collect_tourapi.py` 실행 → `places` 적재(overview 없는 항목 스킵 로그 확인).
- [ ] `build_area_map.py` → `place_area_map` 생성.
- [ ] `build_embeddings.py` → `embeddings.npy` 생성(최초 e5 다운로드).
- [ ] 실데이터로 `/match`·`/schedule`·`/card` 스모크(키 세팅 후 예보 실제 반영 확인).

### 🟠 P1 — Android 3화면 + 연동 (Phase 4)
- [ ] 홈(지역/출발시각/방문지 → `/schedule`), 플래너(타임라인·혼잡 배지·"대안 보기"→`/match`), 대안카드(`/card` 근거 + 원탭 스왑).
- [ ] Retrofit2 + Coroutines, 로딩/에러/재시도. (서버에 CORS/네트워크 정책 확인)

### 🟡 P2 — 모듈4 실시간 스왑 (Phase 4-5)
- [ ] CrowdMap `TrackingCore`(Foreground Service) 재활용 → 다음 장소 실시간 급증 감지 시 `router.resolve_now`로 스왑 제안·푸시.

### 🟡 P2 — 통합·안정화·심사 산출물 (Phase 5)
- [ ] E2E 안정화: API 실패/예보부재/네트워크 끊김에서 **fallback 전 경로** 동작 확인.
- [ ] `valgrind`로 엔진 메모리 누수 점검, 서버 크래시 내성.
- [ ] 시연 시나리오 고정: 경복궁 혼잡 시각 → 대안 궁궐 스왑 → 절약 시간 표시까지 한 번에.
- [ ] 기능설명서(해시태그↔기능, README에 초안 있음) + OpenAPI 필수 활용 근거(TourAPI 명시, 서울데이터는 외부추가) 정리.

---

## 3. 있으면 더 좋을 것 (개선 · 발전성)

### 안정성/품질
- **자동 테스트 통합**: 지금은 수동 스크립트. `ctest`(엔진) + `pytest`(서버, TestClient) + 최소 CI(GitHub Actions)로 회귀 방지.
- **`forecast_cache` 영속화**: 부록 A 테이블을 실제로 채워 재시작/오프라인 데모 시 예보 재현.
- **관측성**: 구조적 로깅·요청 지표(캐시 히트율, 예보 fallback 단계 분포), `/health`에 캐시 통계 추가.
- **좌표/시간 회귀 테스트**: `mapx/mapy` 스왑, KST↔UTC 경계(자정·서머타임 없음) 케이스 고정.

### 성능
- **C++ 코사인 최적화**(로드맵): 후보가 커지면 numpy 대신 엔진에서 반경+코사인 일괄 처리.
- **예보 이웃 fallback 비용 절감**: 키 있고 예보 부재 시 인근 2곳 추가 HTTP → 배치/선캐싱(부팅 시 121지점 예열)으로 지연 완화.
- **수집 재개(resume)**: `collect_tourapi`에 페이지/컨텐츠 커서 저장 → 쿼터·중단 대응.

### 기능 확장
- **전국 예보 확장**: 한국관광 데이터랩 혼잡도 등 추가로 서울 121지점 스코프 탈피(결정 C의 로드맵).
- **실측 라우팅 API**: 현재 `travel()`=haversine÷25km/h 근사, `dwell`=카테고리 기본값 → 대중교통/도보 실측으로 교체.
- **여백 스탬프 리워드**: 발전성 점수용 사용자 리텐션 기능.
- **가중치 튜닝 UI/실험**: `w_c/w_d/w_s`와 고혼잡 임계를 데이터로 최적화.

### 운영/보안
- 서버 **CORS·레이트리밋** 설정(앱 공개 시), 키는 계속 env로만.
- 배포 스크립트/컨테이너화(현재 로컬 uvicorn 기준).

---

## 4. 알아두면 좋은 것 (주의·함정)

- **브랜치**: 이 작업은 **`honey`** 에서 진행. 자동 생성됐던 `claude/code-writing-kodsp8`에만 있던 커밋 5개(`signal/*.kt` 등)는 `honey`에 미포함 — 필요하면 별도 병합.
- **키 없을 때**: 예보는 네트워크 미시도·중립(보통) 폴백. 그래서 매칭/스케줄러/카드는 키 없이도 돌지만, **혼잡도 수치는 전부 2로 고정**되어 스케줄 재정렬/치환이 발동하지 않는다(정상 동작이지 버그 아님).
- **`.so` 재빌드 시점**: `engine/` 헤더를 고치면 반드시 `cmake --build build` 재실행(빌드 산출물은 gitignore, `server/` 옆으로 자동 복사).
- **아직 미검증 가정 1개**: `FCST_TIME` 포맷/예보 지평 — P0에서 실측 전까지는 "동작할 것으로 설계됨" 상태.
- **데이터 산출물은 커밋 안 함**: `data/*.db`, `*.npy`는 스크립트로 재생성(gitignore).
