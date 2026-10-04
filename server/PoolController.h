#ifndef POOL_CONTROLLER_H
#define POOL_CONTROLLER_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ClassPool.h"

/**
 * @brief 요청 종류별 큐 상한 K, 목표 길이 L, 활성 스레드 수 c 를 10ms 마다 정한다.
 *
 *   ŝ  = 0.7ŝ + 0.3·(도착 − 꺼냄)/Δt          // 여유분 감소 속도 (shed 중에도 초과 유입을 셈)
 *   λ̂, μ̂, Cs² = 1초 지수 평활 (μ̂ = 1/E[S], S = 벽시계 처리 시간)
 *   L  = c·μ̂·50ms,  K = clamp(c·μ̂·1s, 64, 2¹⁷)
 *   c_ss = P(Wq > 50ms) ≤ 1% 이고 ρ ≤ 0.9 인 최소 c   (Erlang C + Cs² 보정, λ̂ 사용)
 *   Q'   = Q + ŝ_d·100ms,  ŝ_d = |ŝ| > 3σ_ŝ ? ŝ : 0   (무작위 변동은 버스트로 보지 않음)
 *   c_b  = Q' > L ? c + ⌈(ŝ_d + (Q' − L)/0.5s)/μ̂⌉ : 0   (유체 근사)
 *   want = clamp(max(c_ss, c_b), c_min, c_max)
 *   K_floor(ρ̂) > K 이면 want + 1 (1초 안에 무작위 변동을 흡수할 수 없음)
 *   늘릴 땐 즉시, 줄일 땐 Q < L/2 가 200ms 이어질 때마다 1개씩
 *   늘린 뒤 확인 창(≥ 100ms) 동안 처리량 증가 < 0.5·μ̂·(늘린 수) → c_max = 늘리기 전 c
 *   (남는 물리 코어가 없다는 뜻. 10초 뒤 원래 c_max 로 복구)
 */
struct ControllerParams {
    double tickSec        = 0.010;   // 제어 주기
    double waitTargetSec  = 0.050;   // 대기 목표 (L, c_ss)
    double waitTailP      = 0.01;    // p99
    double waitMaxSec     = 1.0;     // 대기 상한 (K)
    double slopeAlpha     = 0.3;     // ŝ 평활 (약 28ms)
    // ŝ 불감대: |ŝ| ≤ zσ 면 0 으로 본다. σ 는 틱당 도착·꺼냄 개수를 포아송으로 보고 계산.
    // SIMPLE(수만/s)은 σ/μ̂ ≈ 0.02 라 영향이 없지만, COMPLEX(수십/s)는 틱당 0~1건이라
    // σ/μ̂ ≈ 0.4 → 불감대가 없으면 도착 1~2건에 c_b 가 반응해 1↔2 를 1~2초마다 오간다.
    double slopeZ         = 3.0;
    double rateTauSec     = 1.0;     // λ̂·μ̂ 평활 시간
    double horizonSec     = 0.100;   // 예측 시간
    double drainSec       = 0.5;     // 적체 해소 목표
    double rhoMax         = 0.9;     // 최대 이용률
    double shrinkHoldSec  = 0.200;   // 축소 대기
    double gainWindowSec  = 0.100;   // 증설 효과 확인 시간(최소)
    double gainMin        = 0.5;     // 증설 효과 기준 (× μ̂)
    // 확인 창은 완료 건수의 무작위 변동이 기준 폭보다 z 배 작아질 만큼 길게 잡는다:
    //   T ≥ z²·c₁ / (gainMin²·(c₁−c₀)²·μ̂)   (완료 수 ≈ 포아송)
    // SIMPLE(μ̂ 3만/s) 2ms → 최소 100ms 적용, COMPLEX(μ̂ 100/s) 0.72s.
    // 100ms 고정이면 COMPLEX 는 창 안 완료가 20건뿐이라 오차(±45/s)가 기준(50/s)과 같아 오판한다.
    double gainZ          = 3.0;
    double cMaxRestoreSec = 10.0;    // 낮춘 c_max 복구 (버스트 폭 3초 < 10초 < 주기 25초)
    double kFloorEps      = 1e-4;    // K_floor 의 버림 확률
    double ca2            = 1.0;     // 도착 변동 (다수 클라이언트 합 → 포아송)
    size_t kMin           = 64;
    size_t kMax           = size_t(1) << 17;
};

class PoolController {
public:
    struct ClassConfig {
        ClassPool* pool        = nullptr;
        double     mu0         = 1.0;     // 처리 표본이 없을 때 쓰는 μ (스레드당 /s) — 실험 0 보정값
        double     cs2Prior    = 1.0;
        bool       adaptive    = true;    // false: c 고정 (계산은 기록만)
        bool       dynamicCap  = true;    // false: K = fixedCap
        size_t     fixedCap    = 10'000;
        int        cMin        = 1;
        int        cMax        = 1;       // 물리 워커 코어 수 (adaptive) 또는 고정 스레드 수
    };

    PoolController(std::vector<ClassConfig> classes, ControllerParams params,
                   const std::string& csvPath, std::vector<int> cpus);
    ~PoolController();

    void start();
    void stop();

private:
    struct State {
        ClassConfig  cfg;
        PoolSnapshot prev;
        bool         primed     = false;
        double       sHat       = 0.0;
        double       lamHat     = 0.0;
        double       nEma       = 0.0;   // 처리 건수 EMA (μ̂, Cs² 의 가중치)
        double       s1Ema      = 0.0;
        double       s2Ema      = 0.0;
        double       meanS      = 0.0;   // 마지막 유효 E[S]
        double       cs2        = 1.0;
        int          c          = 1;
        int          cMaxEff    = 1;
        double       cMaxCutAt  = -1.0;
        double       shrinkFrom = -1.0;  // Q < L/2 가 시작된 시각 (−1 = 아님)
        // 증설 효과 확인
        bool         gainPending = false;
        int          gainC0      = 0;
        int          gainC1      = 0;
        double       gainT0      = 0.0;
        double       gainWin     = 0.0;
        uint64_t     gainDone0   = 0;
        double       gainMu      = 0.0;
        size_t       gainMinQ    = 0;
    };

    struct Decision {
        double lam = 0, mu = 0, cs2 = 0, sHat = 0, sSigma = 0, rho = 0, kFloor = 0;
        size_t q = 0, k = 0;
        double l = 0;
        int    cSs = 0, cB = 0, want = 0;
    };

    void loop();
    void tick(State& st, double nowSec, double dt);
    void writeRow(const State& st, const PoolSnapshot& s, const Decision& d, double nowSec,
                  uint64_t dOff, uint64_t dShed, uint64_t dDone, uint64_t dExp, double meanWait,
                  double meanSvc);

    std::vector<State> states_;
    ControllerParams   p_;
    std::string        csvPath_;
    std::vector<int>   cpus_;
    FILE*              csv_ = nullptr;
    std::atomic<bool>  running_{false};
    std::thread        thread_;
    int64_t            t0Ns_ = 0;
};

#endif // POOL_CONTROLLER_H
