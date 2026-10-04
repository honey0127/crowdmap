# v5 큐·스레드 풀 — 구현과 실험 실행 방법

설계: `design-v5-queue-threadpool.md` (요청 종류별 큐, 1~3 워커 적응, 큐 상한 K = c·μ̂·1초).
이 문서는 코드가 설계를 어떻게 구현했는지, 설계와 달라진 점, 실험 0·A~D 실행 방법을 적는다.

## 1. 구조

```
리액터(코어0) ─ parseLine ─┬─ SIMPLE  ClassPool ──┐
                           ├─ COMPLEX ClassPool ──┴─ 워커 코어 1~3 공유 (COMPLEX nice +5)
                           └─ 캐시 만료 → CongestionRouter REFRESH 큐(구역 중복 제거) → I/O 스레드 1
PoolController(코어0, 10ms): 큐 길이·도착·꺼냄·완료·처리시간 → K, L, 활성 스레드 c
```

| 파일 | 역할 |
|---|---|
| `ClassPool.{h,cpp}` | 종류별 큐 + 스레드 풀. 입장 상한 K(초과 → shed), 스레드 재우기(park), 꺼낼 때 마감 2초 검사, 누적 카운터 |
| `ChunkQueue.h` | 1,024칸 블록을 붙였다 떼는 FIFO. 여분 블록 1개, 비면 1개만 남김 |
| `PoolController.{h,cpp}` | 10ms 컨트롤러. λ̂·μ̂·Cs²·ŝ 추정, c_ss(Erlang C)·c_b(유체), K_floor 점검, 증설 효과 확인, CSV |
| `QueueMath.h` | Erlang C, 대기 꼬리, 최소 서버 수, K_floor (테스트로 설계 문서 수치 재현) |
| `QueryService.{h,cpp}` | 조회 처리·응답, shed/마감 초과 시 옛 캐시 응답, 실험용 합성 CPU 작업 |
| `CongestionRouter.h` | `Sync`(v4: 조회 경로에서 API 호출) / `Async`(REFRESH 큐 → I/O 스레드) |
| `CpuTopology.h` | 물리 코어 탐지, 리액터·워커 코어 배치, 스레드 nice |
| `FakeCongestionClient.h` | 실험 D 가짜 API (고정 지연, 한 번에 1건) |
| `tools/loadgen.cpp` | 부하 생성기 |
| `tests/queue_tests.cpp` | 단위 테스트 |

`ThreadPool.{h,cpp}` 는 삭제했다. v4 동작은 `POOL_PROFILE=legacy` 로 재현한다(아래 2절).

## 2. 빌드와 서버 설정

```bash
cd server
# 운영 빌드 (w=/id= 무시, 응답 형식 v4 와 같음)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# 실험 빌드 (w=<µs> → COMPLEX, id= 에코, 응답에 |q=|s=|x=)
cmake -S . -B build-exp -DCMAKE_BUILD_TYPE=Release -DCROWDMAP_EXPERIMENT=ON && cmake --build build-exp -j
./build-exp/queue_tests          # 단위 테스트
```

서버 환경변수 (기본값 = v5 설계):

| 변수 | 값 | 기본 | 의미 |
|---|---|---|---|
| `POOL_PROFILE` | `legacy` | — | v4 재현 묶음: `shared` + `fixed` 상한 10,000 + `fixed` 워커 max(코어×4, 8) + `sync`. 아래 변수로 개별 덮어쓰기 가능 |
| `POOL_LAYOUT` | `split` / `shared` | split | 종류별 큐 / 공용 큐 1개 |
| `CAP_MODE` | `dynamic` / `fixed` | dynamic | K = clamp(c·μ̂·1초, 64, 2¹⁷) / K = `QUEUE_CAP` |
| `QUEUE_CAP` | 정수 | 10000 | fixed 모드 상한 |
| `SCALING` | `adaptive` / `fixed` | adaptive | 컨트롤러가 c 조절 / c = `WORKERS` |
| `WORKERS` | 정수 | split: 워커 코어 수, shared: max(코어×4, 8) | fixed 모드 스레드 수 (종류마다) |
| `WORKER_MIN` / `WORKER_MAX` | 정수 | 1 / 물리 코어 − 1 | adaptive 범위 |
| `PIN` | `auto` / `0` | auto | 코어 고정 (물리 코어 1개면 자동으로 끔) |
| `REACTOR_CPU` / `WORKER_CPUS` | `0` / `1-3` 형식 | 첫 물리 코어 / 나머지 | 리액터·배경 스레드 코어, 워커 코어 |
| `COMPLEX_NICE` | 정수 | 5 | COMPLEX 스레드 nice |
| `ROUTER_MODE` | `async` / `sync` | async | REFRESH 큐 / 조회 경로에서 API 직접 호출 |
| `FAKE_API_MS` | ms | — | 설정하면 실제 API 대신 가짜 API. `SEOUL_API_KEY` 불필요, 피더 꺼짐 |
| `MU0_SIMPLE` / `MU0_COMPLEX` | /s | 33333 / 100 | 처리 표본이 없을 때의 μ (실험 0 보정값으로 교체) |
| `CTRL_CSV` | 경로 | — | 컨트롤러 10ms 기록 |

시작 로그에 실제 적용값이 한 줄씩 나온다: `CPU: pin=on ... worker_cores=3`, `Pool: layout=split cap=dynamic scaling=adaptive:1-3 ...`.

## 3. 부하 생성기

```bash
./build-exp/crowdmap_loadgen --host <서버> --rho 0.8 --complex-share 0.5 --sync-frac 0.2 --out run1
```

| 옵션 | 기본 | 의미 |
|---|---|---|
| `--rate R` / `--rho X` | — | 평균 도착률(/s) 또는 1코어(×`--cores`) 기준 이용률. rho 는 `--s-simple-us`(30)·`--work-us` 로 λ 계산 |
| `--complex-frac p` / `--complex-share x` | 0 | COMPLEX 비율 또는 CPU 점유율 (0.5 → p 0.30%, 0.9 → 2.6%) |
| `--work-us W --work-dist fixed\|exp` | 10000, fixed | COMPLEX 작업량·분포 (서버가 스레드 CPU 시간으로 소모) |
| `--sync-frac f --sync-width w --sync-period P --sync-offset o` | 0, 3, 25, 0 | 몰림: 주기 P 마다 f 몫이 폭 w 안에 균등. 최대/평균 = f·P/w + 1 − f |
| `--conns N --threads T --outstanding K` | 20000, 4, 8 | 연결 수, 생성 스레드, 연결당 동시 요청 |
| `--warmup --duration --timeout` | 10, 75, 3 | 측정 구간은 예정 송신 시각이 [warmup, warmup+duration) 인 요청 |

- 열린 루프: 예정 시각에 보내고, 지연은 예정 송신 → 수신. `gen_lag p99 > 10ms` 경고가 나오면 생성기 포화라 결과를 버린다.
- 빈 칸이 있는 연결을 못 찾으면 `skip%` 로 따로 센다(생성기 쪽 한계).
- 출력: `<out>_summary.csv` (종류별 sent/ok/shed/late/timeout, 응답 지연·서버 대기(q)·처리(s) 백분위, 도착 분산지수 10ms/100ms/1s), `<out>_ts.csv` (100ms 칸 시계열).

## 4. 실험별 실행

서버 VM(물리 4코어)과 부하 생성기 VM 을 분리한다. 서버 쪽은 `FAKE_API_MS` 또는 실제 키 중 하나.

**실험 0 — 보정.** 종류별 1코어 μ 와 리액터 한계.
```bash
# 서버: 워커 1코어 고정, 기록
WORKER_CPUS=1 SCALING=fixed WORKERS=1 CTRL_CSV=cal_simple.csv FAKE_API_MS=100 ./build-exp/crowdmap_server
# 생성기: 목표 구간의 도착률로 (아래 '구현 중 확인한 사실' 1번 참고)
crowdmap_loadgen --host S --rate 20000 --warmup 5 --duration 30 --out cal_simple
```
`cal_simple.csv` 의 `mu`(스레드당 처리율 = 1/벽시계 처리시간)를 읽어 `MU0_SIMPLE`, 생성기 `--s-simple-us` 에 넣는다. COMPLEX 는 `--complex-share 0.999` 로 같은 방법. 리액터 한계는 `WORKERS=3` 에 `--rate` 를 올려 `shed%`/`gen_lag` 가 생기기 직전 값.

**실험 A — 지금 서버.** `POOL_PROFILE=legacy WORKER_CPUS=1` 로 서버를 띄우고 {rho 0.5, 0.8} × {share 0, 0.5, 0.9} × {sync 0, 0.05, 0.2, 1.0} 중 최대 유입(rate × 최대/평균)이 리액터 한계의 80% 이하인 조합 × 3회.

**실험 B — 비용.** 서버 설정 4개 × {sync 0.2, 1.0} × 5회, 생성기는 `--rho 0.8 --complex-share 0.9`:
- 지금 서버: `POOL_PROFILE=legacy WORKER_CPUS=1`
- 동적 상한 1코어: `POOL_PROFILE=legacy CAP_MODE=dynamic WORKER_CPUS=1`
- 3코어 고정: `SCALING=fixed WORKERS=3`
- 적응 1~3코어: 기본값

평균 활성 스레드는 `CTRL_CSV` 의 `c` 평균, CPU 시간은 `/proc/<pid>/stat` 의 utime+stime 차이.

**실험 C — Erlang C 검증.** `--complex-frac 1 --work-dist exp --rate {50,150,200}`, 서버는 `SCALING=fixed WORKERS={1,2,3}`. 예측 최소 스레드(2/3/3, `queue_tests` 가 확인)와 단순 계산(1/2/3) 중 P(q > 50ms) ≤ 1% 를 처음 만족하는 실측 c 가 어느 쪽인지. 같은 CSV 의 `c_ss` 열이 실행 중 컨트롤러의 예측이다.

**실험 D — 외부 API 분리.** `FAKE_API_MS=100 WORKER_CPUS=1 SCALING=fixed WORKERS=1` 에 `ROUTER_MODE=sync` / `async` × 5회, 생성기는 SIMPLE 만 `--duration 130` (캐시 TTL 60초 만료를 두 번 포함).

## 5. 컨트롤러 CSV 열

`t_s,cls,q,k,l,c,want,c_ss,c_b,c_max,lam,mu,cs2,s_hat,s_sigma,rho,k_floor,d_off,d_shed,d_done,d_exp,wait_ms,svc_us,chunks`

- `q` 대기 중 개수, `k` 입장 상한, `l` 목표 길이, `c` 활성 스레드, `want` 이번 주기 판단, `c_ss`/`c_b` 정상상태·버스트 판단, `c_max` 유효 상한
- `lam` λ̂, `mu` 스레드당 μ̂, `cs2` 처리시간 Cs², `s_hat` 여유분 감소 속도, `s_sigma` 그 무작위 변동 σ, `rho` λ̂/(cμ̂), `k_floor`
- `d_*` 이번 10ms 의 도착·shed·완료·마감 초과, `wait_ms`·`svc_us` 이번 10ms 완료분의 평균 대기·처리 (Little 법칙 검증: 평균 q ≈ λ × 평균 대기)
- `chunks` 큐 메모리 블록 수

## 6. 설계와 달라진 점 (구현 중 발견, 근거 포함)

1. **ŝ 불감대 3σ 추가.** 설계는 ŝ 의 무작위 변동을 "워커 0.02개분이라 무시 가능"으로 봤다. SIMPLE(σ_ŝ/μ̂ ≈ 676/33k)은 맞지만 COMPLEX 는 틱당 도착이 0~1건이라 σ_ŝ/μ̂ ≈ 37/95 ≈ 0.4 다. 스모크 테스트에서 COMPLEX 가 1↔2 를 분당 60회 오갔다. |ŝ| ≤ 3σ_ŝ 면 0 으로 보도록 바꾼 뒤 분당 7.4회. σ_ŝ = √(α/(2−α)·(λ̂ + 처리율)/Δt) (틱당 개수 포아송 가정). 실제 적체 Q 는 그대로 판단에 쓰므로 Q > L 이면 바로 늘린다.
2. **증설 효과 확인 창: 100ms 고정 → T ≥ 3²·c₁ / (0.5²·(c₁−c₀)²·μ̂), 최소 100ms.** COMPLEX(10ms)는 100ms 창에 완료가 약 20건이라 처리량 추정 오차(±45/s)가 기준(0.5·μ̂ ≈ 50/s)과 같다. 스모크에서 빈 코어가 있는데 "효과 부족"으로 판정해 c_max 를 1로 깎았고, 그 10초 사이에 버스트가 와 COMPLEX p99 가 301ms 가 됐다. 바꾼 뒤 47ms, 오판 0회. SIMPLE 은 계산값이 2ms 라 100ms 그대로.
3. **깎은 c_max 를 10초 뒤 복구 (설계에 없던 값).** 복구가 없으면 한 번의 경합(다른 종류가 코어를 쓰던 순간)으로 영구히 상한이 묶인다. 10초 = 버스트 폭 3초보다 길게(한 버스트 안에서 반복 시도 방지), 주기 25초보다 짧게(다음 버스트 전 복구). 확인 필요 항목.
4. **TCP_NODELAY.** 한 연결에 응답이 연달아 나갈 때 Nagle 이 두 번째 응답을 앞 응답의 ACK 까지 잡아 둬 수십 ms 가 측정에 섞인다. 요청-응답 프로토콜이라 서버(accept 소켓)와 생성기 양쪽에 켰다. 앱은 연결당 요청 1개씩이라 영향 없음.
5. **처리시간 두 가지.** 응답의 `s=` 는 송신 직전까지의 계산 시간(SIMPLE 약 2µs), 컨트롤러 μ̂ 는 송신·close 를 포함한 스레드 점유 시간(SIMPLE 약 10~30µs). 용량 계산에는 후자가 맞다.
6. **Async 라우터의 첫 조회.** 처음 보는 구역은 갱신이 끝날 때까지 내부 계산값으로 응답한다(v4 는 API 응답을 기다렸다). 설계의 "조회는 옛 캐시값이나 내부 계산값으로 바로 응답"과 같지만 운영 동작이 바뀌는 부분이다.

## 7. 구현 중 확인한 사실 (이 컨테이너 스모크, 실험 아님)

조건: 4코어 1대에 서버(리액터 CPU0, 워커 CPU1·2)와 생성기(CPU3)를 같이 띄움. 연결 5,000, rho 0.8(30µs 가정), COMPLEX CPU 50%, 동기화 0.2, 측정 25초(버스트 1회).

| 설정 | SIMPLE p99 | COMPLEX p99 | shed | 평균 활성(SIMPLE/COMPLEX) |
|---|---|---|---|---|
| 지금 서버 (legacy, 1코어) | 477~494ms | 651~676ms | 4.7~4.8% | 16 (공용) |
| 3코어 고정 대신 2코어 고정 | 2.0~2.2ms | 49~136ms | 0% | 2 / 2 |
| 적응 1~2 | 1.8~2.4ms | 47ms | 0% | 1.00 / 1.15~1.16 |
| 실험 D: sync 라우터, 워커 1 | p99 420ms | — | 4.95% | — |
| 실험 D: async REFRESH, 워커 1 | p99 1.0ms | — | 0% | — |

1. **SIMPLE μ 는 부하에 따라 변한다.** 스레드 점유 시간이 초당 3천 건에서 약 25µs, 3만 건에서 약 10µs 였다(깨우기 비용 상각, v4 의 55→29µs 와 같은 방향). 30µs 로 계산한 "rho 0.8" 이 실제로는 SIMPLE ρ 0.38 이었다. 실험 0 보정은 목표 도착률 근처에서 해야 한다.
2. 반복 2회뿐이고 생성기가 같은 기계에 있어 수치는 방향 확인용이다. 2코어 고정의 COMPLEX p99 는 두 번이 49ms·136ms 로 흔들렸다.
