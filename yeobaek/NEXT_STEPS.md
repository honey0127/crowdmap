# 여백(Yeobaek) — 앞으로 할 일 백로그 (터미널/Claude Code 작업용)

> 현재까지: 홈 지도(구글맵·히트맵)·검색·어드혹 등록·코스 2모드·지역 플래너(전국 지역구)·
> 대안카드·오프피크·미시분산·에코포인트·실시간 제보·전국 집중률(TatsCnctrRateService) 연동까지 구현.
> 아래는 "실제 배포/공모전 완성도"까지 남은 작업을 우선순위로 정리한 것. 각 항목은
> Claude Code에게 그대로 지시하면 되도록 **무엇을/왜/어디서**로 적었다.

각 작업은 대략 이렇게 진행: `git fetch origin honey && git reset --hard origin/honey` →
Claude Code로 수정 → 서버/앱 확인 → 커밋·푸시.

---

## P0 — 데이터 완성도 (지금 가장 큰 병목)

- [ ] **전국 명소 수집 완주**: `python scripts/collect_tourapi.py --nationwide --pages 8`
  (임베딩 품질 위해 `--no-overview` 빼고 소개글까지). 이후 `build_area_map.py`.
  → 지역구 라인업/히트맵이 전국에서 실제로 채워짐. (지금 강남 라인업이 빈 이유가 이것)
- [ ] **임베딩 생성**: `pip install -r scripts/requirements.txt && python scripts/build_embeddings.py`
  → `/match`·대안카드(감성 쌍둥이) 동작.
- [ ] **집중률 이름매칭률 개선**: TourAPI 제목 ↔ 집중률 `tAtsNm` 정규화/별칭 사전.
  위치: `server/api/districts.py` `_norm`/`_match_level`. contentId가 없어 이름 매칭이 핵심.
- [ ] **지방 signgu 코드 전수 검증**: `DISTRICTS`의 `signgu_cd` 25개를 `/tats`로 확인해 오류 수정.
  위치: `server/api/districts.py`.

## P1 — 혼잡 정확도·소스 통합

- [ ] **집중률 일배치 캐시 테이블화**: 매 요청 API 호출 대신 하루 1회 프리컴퓨트 → `tats_congestion` 테이블.
  스크립트 `scripts/build_tats.py` 신설 + `repository` 조회. (일일 트래픽 1000 절약)
- [ ] **통합 혼잡 프로바이더**: 장소별로 서울=시간별(실시간 도시데이터) / 그 외=일별(집중률) **자동 선택**.
  지금은 지역구 라인업만 집중률 반영 → `/heatmap`·`/schedule`·`/offpeak`에도 전국 반영.
- [ ] **스케줄러에 전국 집중률 주입**: 엔진은 '시간 bucket' 전제 → 도착 '날짜' 레벨을 주입하는 경로 추가.
  위치: `engine/`(ForecastProvider) 또는 서버에서 레벨을 직접 넘기는 인터페이스.
- [ ] **rate→level 임계 튜닝**: `services/tats.py` `rate_to_level` 데이터로 조정.

## P1 — 앱 UX 완성도

- [ ] **플래너 편집**: 순서 드래그(ItemTouchHelper)·삭제·저장/불러오기(로컬 DB/Prefs).
- [ ] **히트맵 시각화 개선**: 핀 색 외에 원형 농도 오버레이(Circle/TileOverlay) + 범례 칩.
- [ ] **오프피크/제보/에코/리스케줄 UI 다듬기**: 지금은 최소구현 → 다이얼로그→화면, 배지 애니메이션 등.
- [ ] **상태 처리 통일**: 로딩/빈상태/에러 컴포넌트, 접근성(콘텐츠 설명), 다크모드 점검.
- [ ] **온보딩**: 첫 실행 3화면 튜토리얼(핵심: 지도 탭→담기→코스, 지역 플래너).

## P1 — 안정성·품질

- [ ] **자동 테스트/CI**: `pytest`(서버 TestClient) + `ctest`(엔진) + GitHub Actions.
  위치: `yeobaek/tests/`(신설), `.github/workflows/`.
- [ ] **외부 API 견고성**: 타임아웃/재시도/서킷브레이커, 캐시 TTL, 실패 시 graceful.
  위치: `services/tats.py`, `external/*`.
- [ ] **관측성**: 구조적 로깅 + 요청 지표(캐시 히트율, API 실패율), `/health` 확장.
- [ ] **엔진 메모리 점검**: `valgrind`로 누수 확인, 크래시 내성.

## P2 — 🔴 외부 소스 연동 (키/접근 확보 시 — 확보되면 Claude Code로 연동)

- [ ] **서울 지하철 실시간 혼잡도**(data.seoul.go.kr OA-12928) → 교통 분산·리스케줄 보강.
- [ ] **CCTV/웹캠**(ITS its.go.kr) → 명소 실시간 상태 링크.
- [ ] **대중교통 혼잡 반영 길찾기**(교통 API) → '덜 붐비는 경로'.
- [ ] **예약 타임슬롯 통합**(예약처 제휴 API).
- [ ] **실제 쿠폰/상점 제휴**(에코 리워드 정산) — 지금은 로컬 포인트만.
- [ ] Google 인기시간대 = 공식 API 없음 → 집중률로 대체(이미 반영).

## P2 — 배포·운영

- [ ] **서버 배포**: 도커라이즈 + 클라우드(무료 티어), HTTPS, CORS, 레이트리밋.
- [ ] **앱 릴리스**: 서명·release 빌드, 지도키 제한(SHA-1+패키지), 스토어/공모전 제출 패키지.
- [ ] **비밀 관리**: 키를 `.env`/시크릿으로만(코드/깃 금지 — 이미 준수).

## P3 — 발전 기능 (원래 15개 비전 중 남은 것)

- [ ] 크라우드소싱 고도화: 제보 신뢰도/신고/사진 첨부.
- [ ] 역방향 동선 고도화: 실측 라우팅(대중교통/도보) 반영.
- [ ] 탄소발자국·지역소득 분산 시각화(근사 모델).
- [ ] 게이미피케이션 확장: 배지 종류/랭킹/주간 챌린지.
- [ ] 현지인 가이드 팁(에티켓) 등록·노출.

---

## 이번 주 추천 순서 (막히면 이대로)

1. `collect_tourapi.py --nationwide --pages 8` + `build_area_map.py` → 데이터 채우기
2. `/districts/gangnam` 이 채워지는지 확인 → 이름매칭률 보고 `_match_level` 개선
3. 집중률 **캐시 테이블화**(build_tats.py) → 트래픽 절약 + 속도
4. **통합 혼잡 프로바이더**로 `/heatmap`·`/schedule`까지 전국 혼잡 반영
5. 플래너 편집 + 히트맵 시각화로 앱 완성도 ↑
6. pytest + CI로 회귀 방지

> Claude Code 사용 팁: 위 항목 하나를 그대로 붙여넣고 "이거 구현해줘"라고 하면 됨.
> 시작 전 항상 `git fetch origin honey && git reset --hard origin/honey` 로 최신화.
