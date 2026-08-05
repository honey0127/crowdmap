# 데이터 파이프라인 검증 결과 (Phase 1)

> 목적: "코드는 있으나 한 번도 흐른 적 없는" 데이터 파이프라인을 실제로 돌려,
> 가정 위에 쌓인 부분과 실측으로 확인된 부분을 분리한다.
> 재현: `cd yeobaek && python scripts/validate_pipeline.py`

## TL;DR

- **엔진**: fresh clone 에서 `engine/` CMake 빌드 → import → `nearest_area` 동작까지 재확인. ✅
- **스케줄러 코어**: 단위테스트 재컴파일·실행 → "43% 절감" + 치환 로직 결정적 재현. ✅
- **build_area_map (지오 매핑)**: 실 C++ 엔진으로 실행. 주요 서울 관광 핫스팟 18곳 **전부 ≤1.6km 예보지점 매핑(커버리지 100%)**. ✅
- **서빙 계층**: `/health /match /schedule /card /places/search` 전부 200 E2E 통과 (시드 DB 기준). ✅
- **키 없을 때 폴백**: 예보 클라이언트가 네트워크 실패 시 중립 레벨로 폴백하는 것 확인(로그 `[Forecast] Empty response … → 중립`). ✅
- **실측 fetch 2단계는 이 환경에서 실행 불가** — 아래 "환경 제약" 참고. ⛔

## 환경 제약 (이 샌드박스에서 실행 불가한 것)

실서비스 데이터 완주에는 **API 키**와 **외부 egress** 둘 다 필요한데, 이 실행 환경에는 둘 다 없다.

| 단계 | 필요 조건 | 이 환경 | 결과 |
|---|---|---|---|
| `collect_tourapi.py` | `TOURAPI_KEY` + `data.go.kr` egress | 키 없음 · egress 정책상 `apis.data.go.kr` 차단(HTTP 000) | 실행 불가 |
| `build_embeddings.py` | `sentence-transformers` + `huggingface.co` 모델 다운로드 | egress 정책상 `huggingface.co` 차단(CONNECT 403) | 실행 불가 |
| `build_area_map.py` | 빌드된 C++ 엔진 + places 좌표 (키·외부망 불필요) | 조건 충족 | **실행됨 ✅** |

> egress 차단은 조직 네트워크 정책이므로 이 환경에서 우회하지 않는다.
> 두 단계는 **키가 있고 한국 공공 API·huggingface 접근이 열린 개발망**에서 실행해야 한다.

## 실측으로 확인된 것

### build_area_map — 서울 121 예보지점 커버리지 (검증 질문 Q2)

실측 좌표를 가진 대표 서울 명소 18곳(궁궐·전통마을·번화가·공원·미술관)을 시드해
실제 C++ `SeoulCityDataForecastClient::nearest_area` 로 매핑한 결과:

- **18/18 전부 매핑, 전부 ≤1.6km** (대부분 수백 m 이내).
- 결론: **주요 관광 핫스팟에 대한 121지점 커버리지는 우수**하다. 결정 C의 "매핑 없으면 중립 폴백"이
  핫스팟에서 발동할 일은 거의 없다.
- 단, 이는 fixture 18곳 기준이다. 전국/외곽까지 포함한 **전체 수집본의 실제 커버리지·스킵률(Q1)은
  `collect_tourapi.py` 완주 후에야 확정**된다 (아래 "다음 단계").

일부 매핑 예시(가장 가까운 예보지점):

```
경복궁        → 경복궁            (0.00 km)
덕수궁        → 덕수궁길·정동길      (0.09 km)
북촌한옥마을    → 북촌한옥마을        (0.19 km)
코엑스        → 강남 MICE 관광특구   (0.01 km)
예술의전당     → 교대역            (1.56 km)   ← fixture 중 최대 거리
```

### find_twins / 서빙 랭킹 배관

`SpatialIndex 반경후보(C++) ∩ 코사인 top-K(numpy) ∩ 예보지점 결합` 흐름이 정상 동작.
`/match` 로 경복궁 요청 시 상위 3개가 같은 성격 군집(궁궐)으로 반환됨을 확인.

## 검증되지 **않은** 것 (개발망에서 확인 필요)

- **Q1 — overview 스킵률 / 후보 풀 크기**: `collect_tourapi.py` 완주 필요. 미측정.
- **Q3 — e5-small 코사인 top-3 의 "감성 쌍둥이" 의미 품질**: huggingface 차단으로
  실제 e5 모델을 못 돌린다. 검증 하네스는 **구조적 임베딩 스탠드인**(같은 군집=코사인↑)을 써서
  *랭킹 배관* 만 검증했고, *의미 품질* 은 검증 대상이 아니다. e5 가 실제로 경복궁≈덕수궁을
  가깝게 보는지는 `build_embeddings.py` 로 개발망에서 눈으로 확인해야 한다.
- **전체 예보지점 커버리지(Q2 전량)**: fixture 18곳이 아닌 전체 수집본 기준 비율.

## 다음 단계 — 개발망(키+외부망)에서 실행

```bash
export TOURAPI_KEY=<디코딩키> SEOUL_API_KEY=<키>
pip install -r scripts/requirements.txt        # sentence-transformers(torch 포함)

python scripts/collect_tourapi.py --pages 5     # data.go.kr egress 필요 → places 채움
python scripts/build_area_map.py                # ← 이 단계 배관은 이미 검증됨(위)
python scripts/build_embeddings.py              # huggingface egress 필요 → embeddings.npy

# 이후 서버:  uvicorn server.main:app --reload
```

완주 후 확인할 3개 숫자(원래 검증 목표):
1. overview 남는 장소 수 / 스킵률 (Q1)
2. 전체 수집본의 서울 예보지점 매핑 비율 (Q2 전량)
3. e5 top-3 가 사람 눈에 "감성 쌍둥이"로 보이는지 (Q3)

## 검증 하네스

`scripts/validate_pipeline.py` — 키/외부망 없이 파이프라인 배관 전체를 실제 프로덕션 코드로 실행.
격리된 임시 DB(`/tmp`)를 쓰므로 레포/데이터를 오염시키지 않는다. CI 에도 그대로 물릴 수 있다.
현재 **12/12 통과**.

fixture: `scripts/fixtures/seoul_places.json` (실측 좌표·소개문, 임베딩 군집 라벨 포함).
