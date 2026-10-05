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
| `POOL_PROFILE` | `legacy` | — | v4 재현 묶음: `shared` + `fixed` 상한 10,000 + `fixed` 워커 max(코어×4, 8) + `sync` + 마감 끔. 아래 변수로 개별 덮어쓰기 가능 |
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
| `DEADLINE_MS` | ms | 2000 (legacy 0) | 꺼낼 때 이보다 오래 기다린 요청은 처리 없이 옛 캐시 응답(x=D). 0 = 끔(v4 는 마감 없음) |
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
| `--q-threshold-ms T` | 50 | 서버 큐 대기 q > T 인 응답 수를 센다(`q_over`, 실험 C 판정) |
| `--min-conn-frac F` | 0.99 | 연결 성공률이 F 미만이면 부하를 걸지 않고 종료 코드 3 |

- 열린 루프: 예정 시각에 보내고, 지연은 예정 송신 → 수신. `gen_lag p99 > 10ms` 경고가 나오면 생성기 포화라 결과를 버린다.
- 빈 칸이 있는 연결을 못 찾으면 `skip%` 로 따로 센다(생성기 쪽 한계).
- 출력: `<out>_summary.csv` (종류별 sent/ok/shed/late/timeout, 응답 지연·서버 대기(q)·처리(s) 백분위, `q_over`, `rest` = 응답 지연 − q − s(네트워크·리액터·생성기 몫), 도착 분산지수 10ms/100ms/1s, 측정 시작 벽시계 `t0_epoch`), `<out>_ts.csv` (100ms 칸 시계열).
- q 분포에는 마감 초과(x=D) 응답의 대기도 들어간다(서버가 x=D 에 q= 를 붙인다). 3초 안에 응답이 없는 요청은 timeout 으로만 센다.
- 연결은 모두 RST 로 닫는다(SO_LINGER 0). FIN 으로 닫으면 연결마다 TIME_WAIT 가 60초 남아 바로 다음 실행이 임시 포트를 다 못 얻는다.

## 4. 실험 실행 스크립트 (`experiments/`)

| 파일 | 역할 |
|---|---|
| `exp0_calibrate.sh` | 보정 → `results/calib.env`: SIMPLE 처리 시간-도착률 표(워커 1코어), COMPLEX 처리 시간, 리액터 한계 |
| `expA.sh` ~ `expD.sh` | 실험 하나: 계획 → 사전 점검 → 실행 → 보고서 |
| `run_all.sh [A B C D]` | 보정(없으면) → A → B → C → D → `results/REPORT.md` |
| `plan.py` | 보정값으로 도착률 계산(1코어 기준 이용률 ρ), 조건표, 리액터 한계 제외, 실행 순서 |
| `analyze.py` | 보정 판정, 실험별 `runs.csv`·`summary.csv`·`summary.md`, `REPORT.md` |
| `lib.sh`, `expcommon.py` | 공용 함수 |
| `exp.conf.example` | 설정 예 — `exp.conf` 로 복사해 고친다(기계별이라 git 에서 뺌) |

### 준비
1. 서버 VM(물리 4코어)과 부하 생성기 VM 두 대에 각각 실험 빌드(2절)를 만든다.
2. 생성기 VM → 서버 VM ssh 키 접속(비밀번호 없이)이 되게 한다.
3. `experiments/exp.conf` 를 만든다: `SERVER_SSH`, `SERVER_HOST`, `SERVER_BIN`(서버 VM 경로) 은 꼭 채운다.
4. fd 한도: 생성기 hard nofile ≥ 연결 수 + 1000, 서버 ≥ 연결 수 + 131072 + 1000 (큐에 쌓인 작업마다 dup fd 를 하나씩 쥔다). 생성기 임시 포트 범위 ≥ 연결 수 + 1000. 사전 점검이 확인하고, 모자라면 멈춘다.

### 실행
```bash
cd server/experiments
DRY_RUN=1 ./run_all.sh     # 실행 목록·제외 조건·예상 시간만 (보정 파일이 있어야 A~D 계획이 나온다)
./run_all.sh               # 전체 (기본값으로 보정 약 15분 + A 50분 + B 67분 + C 72분 + D 25분)
./expC.sh                  # 실험 하나만
```
- 실행마다 서버를 새로 띄우고 끝나면 SIGTERM 으로 멈춘 뒤 컨트롤러 CSV·로그를 가져온다(상태가 이어지지 않게).
- 같은 반복 안에서는 조건을 고정 시드로 섞어 돌린다(시간에 따른 변화가 한 조건에 몰리지 않게). 생성기 시드는 반복마다 같다 → B 처럼 설정만 다른 비교는 같은 도착 순서로 짝지어진다.
- **이어하기**: 같은 명령을 다시 실행하면 완료(DONE)된 실행은 건너뛴다. 단, 서버 env·생성기 인자·두 바이너리의 서명이 지금 계획과 다르면 예전 결과를 `_stale/` 로 옮기고 다시 잰다. 보고서는 지금 계획에 있는 실행만 쓴다.
- 실패한 실행은 `FAILED` 를 남기고 다음으로 간다. 연속 3번 실패하면 환경 문제로 보고 멈춘다. 보정은 한 단계라도 두 번 실패하면 `calib.env` 를 쓰지 않고 멈춘다.
- Ctrl-C: 진행 중인 실행을 바로 멈추고 서버를 정지한다. 그 실행은 이어하기 때 다시 잰다.
- 보정을 다시 재려면 `results/calib.env` 와 `results/exp0` 를 함께 지운다.
- 같은 기계 스모크: `exp.conf.example` 의 주석대로 `SERVER_ENV_COMMON`·`LOADGEN_TASKSET` 으로 코어를 나누고 창을 짧게 잡는다.

### 실험별 내용
| 실험 | 서버 | 부하 | 반복 | 보고서에서 볼 것 |
|---|---|---|---|---|
| 0 | `SCALING=fixed WORKERS=1 WORKER_CPUS=1` (SIMPLE·COMPLEX), `SCALING=fixed`(리액터) | SIMPLE 1천~8만/s 올려 가며(이용률 0.85·shed·생성기 포화에서 멈춤), COMPLEX 30/s, 리액터 2만~14만/s | 1 | `calib.env` 와 단계별 판정 |
| A | `POOL_PROFILE=legacy WORKER_CPUS=1` | ρ {0.5, 0.8} × COMPLEX CPU {0, 0.5, 0.9} × 몰림 {0, 0.05, 0.2, 1.0}, 최대 유입 > 리액터 한계 × 0.8 은 제외 | 3 | 조건별 shed / late / timeout, 판정(부족·과다·여유), 실측 워커 CPU vs ρ(계획) |
| B | legacy / legacy+동적 상한 / 종류별 3개 고정 / 적응 1~3 | ρ 0.8, COMPLEX CPU 0.9 × 몰림 {0.2, 1.0} | 5 | q·응답 p99, shed, 종류별 활성 스레드, 워커 CPU, 설계 예측 |
| C | `SCALING=fixed WORKERS={1,2,3} CAP_MODE=fixed QUEUE_CAP=1000000` | COMPLEX 만, 지수분포 10ms, {50, 150, 200}/s, 연결 500 | 5 | P(q > 50ms 또는 실패) 실측 vs M/M/c 예측, 최소 c: 실측·Erlang C·단순 계산 |
| D | `FAKE_API_MS=100 SCALING=fixed WORKERS=1 WORKER_CPUS=1 CAP_MODE=fixed QUEUE_CAP=10000` + `ROUTER_MODE` sync/async | SIMPLE 4000/s, 측정 130초(캐시 TTL 60초 만료 2번) | 5 | p99·p99.9·최대, 튐 칸 수와 시각 |

- ρ(1코어 기준 이용률)는 보정 표의 처리 시간 S(λ) 로 λ·E[S(λ)] = ρ 가 되는 λ 를 고정점 반복으로 푼다. SIMPLE 처리 시간이 도착률에 따라 바뀌기 때문이다(7절). 필요한 도착률이 보정 범위 밖이면 계획 로그에 "주의"로 남긴다.
- 보정은 v5 split 풀(스레드 1개)로 잰다. legacy(스레드 16개, 공용 큐)는 같은 λ 에서 워커 CPU 를 더 쓸 수 있어, A 보고서에 실측 워커 CPU 를 ρ 옆에 함께 싣는다.
- 결과 지표는 실행별 값(예: 실행마다의 p99)의 반복 평균 ± 95% 신뢰구간(t 분포)이다. 생성기 송신 지연 p99 > 10ms 인 실행은 평균에서 빼고 보고서에 목록을 남긴다.

## 5. 컨트롤러 CSV 열

`t_s,cls,q,k,l,c,want,c_ss,c_b,c_max,lam,mu,cs2,s_hat,s_sigma,rho,k_floor,d_off,d_shed,d_done,d_exp,wait_ms,svc_us,chunks,epoch_s,cpu_s,reactor_cpu_s,proc_cpu_s`

- `q` 대기 중 개수, `k` 입장 상한, `l` 목표 길이, `c` 활성 스레드, `want` 이번 주기 판단, `c_ss`/`c_b` 정상상태·버스트 판단, `c_max` 유효 상한
- `lam` λ̂, `mu` 스레드당 μ̂, `cs2` 처리시간 Cs², `s_hat` 여유분 감소 속도, `s_sigma` 그 무작위 변동 σ, `rho` λ̂/(cμ̂), `k_floor`
- `d_*` 이번 10ms 의 도착·shed·완료·마감 초과, `wait_ms`·`svc_us` 이번 10ms 완료분의 평균 대기·처리 (Little 법칙 검증: 평균 q ≈ λ × 평균 대기)
- `chunks` 큐 메모리 블록 수
- `epoch_s` 서버 벽시계(생성기의 `t0_epoch` 와 맞춰 측정 구간을 자른다. 두 VM 의 차이는 스크립트가 ssh 로 재서 보정), `cpu_s` 이 풀 워커 스레드들의 누적 CPU 초, `reactor_cpu_s` 리액터 스레드 누적 CPU 초, `proc_cpu_s` 프로세스 전체

## 6. 설계와 달라진 점 (구현 중 발견, 근거 포함)

1. **ŝ 불감대 3σ 추가.** 설계는 ŝ 의 무작위 변동을 "워커 0.02개분이라 무시 가능"으로 봤다. SIMPLE(σ_ŝ/μ̂ ≈ 676/33k)은 맞지만 COMPLEX 는 틱당 도착이 0~1건이라 σ_ŝ/μ̂ ≈ 37/95 ≈ 0.4 다. 스모크 테스트에서 COMPLEX 가 1↔2 를 분당 60회 오갔다. |ŝ| ≤ 3σ_ŝ 면 0 으로 보도록 바꾼 뒤 분당 7.4회. σ_ŝ = √(α/(2−α)·(λ̂ + 처리율)/Δt) (틱당 개수 포아송 가정). 실제 적체 Q 는 그대로 판단에 쓰므로 Q > L 이면 바로 늘린다.
2. **증설 효과 확인 창: 100ms 고정 → T ≥ 3²·c₁ / (0.5²·(c₁−c₀)²·μ̂), 최소 100ms.** COMPLEX(10ms)는 100ms 창에 완료가 약 20건이라 처리량 추정 오차(±45/s)가 기준(0.5·μ̂ ≈ 50/s)과 같다. 스모크에서 빈 코어가 있는데 "효과 부족"으로 판정해 c_max 를 1로 깎았고, 그 10초 사이에 버스트가 와 COMPLEX p99 가 301ms 가 됐다. 바꾼 뒤 47ms, 오판 0회. SIMPLE 은 계산값이 2ms 라 100ms 그대로.
3. **깎은 c_max 를 10초 뒤 복구 (설계에 없던 값).** 복구가 없으면 한 번의 경합(다른 종류가 코어를 쓰던 순간)으로 영구히 상한이 묶인다. 10초 = 버스트 폭 3초보다 길게(한 버스트 안에서 반복 시도 방지), 주기 25초보다 짧게(다음 버스트 전 복구). 확인 필요 항목.
4. **TCP_NODELAY.** 한 연결에 응답이 연달아 나갈 때 Nagle 이 두 번째 응답을 앞 응답의 ACK 까지 잡아 둬 수십 ms 가 측정에 섞인다. 요청-응답 프로토콜이라 서버(accept 소켓)와 생성기 양쪽에 켰다. 앱은 연결당 요청 1개씩이라 영향 없음.
5. **처리시간 두 가지.** 응답의 `s=` 는 송신 직전까지의 계산 시간(SIMPLE 약 2µs), 컨트롤러 μ̂ 는 송신·close 를 포함한 스레드 점유 시간(SIMPLE 약 10~30µs). 용량 계산에는 후자가 맞다.
6. **Async 라우터의 첫 조회.** 처음 보는 구역은 갱신이 끝날 때까지 내부 계산값으로 응답한다(v4 는 API 응답을 기다렸다). 설계의 "조회는 옛 캐시값이나 내부 계산값으로 바로 응답"과 같지만 운영 동작이 바뀌는 부분이다.

7. **legacy 는 마감 검사를 끈다(`DEADLINE_MS=0`).** 처음 구현은 legacy 에도 v5 의 2초 마감이 남아 있어, 대기 2초를 넘긴 작업을 처리 없이 버렸다. v4 는 마감 없이 다 처리했으므로 기준이 v4 가 아니었고, q p99 가 2초에서 잘려 설계 예측(2,834ms)과 비교할 수 없었다. 마감 초과 응답에도 기다린 시간(q=)을 붙여 q 분포가 잘리지 않게 했다.
8. **로그를 줄마다 비운다.** 표준출력이 파일·파이프면 완전 버퍼링이라 `start.sh` 의 tee 에도 로그가 4KB 단위로 늦게 보였다.

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
3. **보정 스모크(같은 기계, 창 5초).** SIMPLE 처리 시간(워커 1코어, 스레드 점유 시간, 두 번 잰 값): 2천/s 26.0·23.6µs, 1만/s 16.1·14.9µs, 3만/s 9.7·8.7µs, 6만/s 6.4·5.9µs — 실행 사이 차이 약 10%, 6만/s 에서도 워커 코어 이용률 0.36~0.38. COMPLEX(작업 10ms) 10.18·10.13ms. 리액터는 6만/s 까지 통과(사용률 0.49, 생성기를 같은 기계에 둔 한계라 하한값).
4. **그래서 A 의 SIMPLE 위주 조건은 워커가 아니라 리액터가 먼저 찬다.** 위 보정값으로 ρ 0.8·SIMPLE 만이 되려면 약 12.5만/s 가 필요해 리액터 한계를 넘는다. 기본 설정의 A 는 24조건 중 10개만 남았다(설계 예상 19개). 실제 VM 보정값으로 다시 정해지지만, "워커 1코어를 SIMPLE 로 포화시키는 조건"은 리액터 1개로는 만들기 어렵다는 방향은 유지될 가능성이 크다.
5. **측정 경로 검증: M/M/1 재현.** COMPLEX 만, 지수분포 10ms, 50/s, 워커 1개로 150초씩 2회: P(q > 50ms) 실측 4.03%·4.36%, M/M/1 예측 ρ·e^{−(μ−λ)·0.05} = 4.34%·4.49%. 8초짜리 스모크에서는 0.56% 로 크게 어긋났는데, 50ms 초과가 긴 바쁜 구간에 몰려 나오므로 짧은 창의 표본 수가 실제로는 몇 개 묶음뿐이기 때문이다 — 실험 C 는 75초 × 5회가 필요하다.
