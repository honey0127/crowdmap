#include "PoolController.h"
#include "CpuTopology.h"
#include "Logger.h"
#include "QueueMath.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace {

size_t clampCap(double k, const ControllerParams& p) {
    if (!(k > 0.0)) k = 0.0;
    if (k > static_cast<double>(p.kMax)) return p.kMax;
    return std::max(p.kMin, static_cast<size_t>(k));
}

}  // namespace

PoolController::PoolController(std::vector<ClassConfig> classes, ControllerParams params,
                               const std::string& csvPath, std::vector<int> cpus)
        : p_(params), csvPath_(csvPath), cpus_(std::move(cpus)) {
    for (auto& cfg : classes) {
        State st;
        st.cfg     = cfg;
        st.c       = cfg.pool->active();
        st.cMaxEff = std::max(cfg.cMin, cfg.cMax);
        st.meanS   = cfg.mu0 > 0 ? 1.0 / cfg.mu0 : 1.0;
        st.cs2     = cfg.cs2Prior;
        states_.push_back(st);
    }
    if (!csvPath_.empty()) {
        csv_ = std::fopen(csvPath_.c_str(), "w");
        if (!csv_) {
            Log::warn("[Controller] CSV 파일 열기 실패: " + csvPath_);
        } else {
            std::fprintf(csv_,
                         "t_s,cls,q,k,l,c,want,c_ss,c_b,c_max,lam,mu,cs2,s_hat,s_sigma,rho,k_floor,"
                         "d_off,d_shed,d_done,d_exp,wait_ms,svc_us,chunks,"
                         "epoch_s,cpu_s,reactor_cpu_s,proc_cpu_s\n");
        }
    }
}

PoolController::~PoolController() {
    stop();
    if (csv_) std::fclose(csv_);
}

void PoolController::start() {
    if (running_.exchange(true)) return;
    t0Ns_   = monoNowNs();
    thread_ = std::thread([this] { loop(); });
}

void PoolController::stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (csv_) std::fflush(csv_);
}

void PoolController::loop() {
    if (!cpus_.empty()) cpu::pinThisThread(cpus_);

    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(p_.tickSec));
    auto    next   = clock::now();
    int64_t prevNs = monoNowNs();
    int     ticks  = 0;

    while (running_.load(std::memory_order_relaxed)) {
        next += period;
        std::this_thread::sleep_until(next);
        // 오래 멈췄다 깨어나면(디버거·서스펜드) 밀린 주기를 몰아서 돌지 않는다
        if (clock::now() - next > std::chrono::seconds(1)) next = clock::now();

        const int64_t nowNs = monoNowNs();
        const double  dt    = static_cast<double>(nowNs - prevNs) * 1e-9;
        prevNs              = nowNs;
        if (dt <= 0.0) continue;

        const double nowSec = static_cast<double>(nowNs - t0Ns_) * 1e-9;
        if (csv_) {
            auto secOf = [](clockid_t cid) {
                timespec ts;
                if (clock_gettime(cid, &ts) != 0) return 0.0;
                return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
            };
            epochSec_      = secOf(CLOCK_REALTIME);
            procCpuSec_    = secOf(CLOCK_PROCESS_CPUTIME_ID);
            reactorCpuSec_ = hasReactorClock_ ? secOf(reactorClock_) : 0.0;
        }
        for (auto& st : states_) tick(st, nowSec, dt);

        if (csv_ && ++ticks % 100 == 0) std::fflush(csv_);
    }
}

void PoolController::tick(State& st, double now, double dt) {
    ClassPool&         pool = *st.cfg.pool;
    const PoolSnapshot s    = pool.snapshot();
    if (!st.primed) {
        st.prev   = s;
        st.primed = true;
        return;
    }

    const uint64_t dOff  = s.offered - st.prev.offered;
    const uint64_t dShed = s.shed - st.prev.shed;
    const uint64_t dDeq  = s.dequeued - st.prev.dequeued;
    const uint64_t dDone = s.done - st.prev.done;
    const uint64_t dExp  = s.expired - st.prev.expired;
    const double   dSvc  = s.svcSum - st.prev.svcSum;
    const double   dSvc2 = s.svcSqSum - st.prev.svcSqSum;
    const double   dWait = s.waitSum - st.prev.waitSum;
    st.prev = s;

    // ── 추정 ────────────────────────────────────────────────────────
    // 여유분 감소 속도: 도착 − 꺼냄. 큐가 차서 shed 중일 때도(ΔQ = 0) 초과 유입을 센다.
    const double inst = (static_cast<double>(dOff) - static_cast<double>(dDeq)) / dt;
    st.sHat = (1.0 - p_.slopeAlpha) * st.sHat + p_.slopeAlpha * inst;

    const double ar = 1.0 - std::exp(-dt / p_.rateTauSec);
    st.lamHat = (1.0 - ar) * st.lamHat + ar * (static_cast<double>(dOff) / dt);
    st.nEma   = (1.0 - ar) * st.nEma + ar * static_cast<double>(dDone);
    st.s1Ema  = (1.0 - ar) * st.s1Ema + ar * dSvc;
    st.s2Ema  = (1.0 - ar) * st.s2Ema + ar * dSvc2;
    if (st.nEma > 1e-9 && st.s1Ema > 0.0) {
        // 표본 가중 평균. 표본이 끊기면 분자·분모가 같이 줄어 마지막 값이 유지된다.
        const double m1 = st.s1Ema / st.nEma;
        const double m2 = st.s2Ema / st.nEma;
        st.meanS = m1;
        st.cs2   = std::max(0.0, m2 / (m1 * m1) - 1.0);
    } else if (st.nEma <= 1e-12) {
        st.nEma = st.s1Ema = st.s2Ema = 0.0;   // 오래 한가하면 비정규 수로 내려가기 전에 정리
    }

    const double mu  = st.meanS > 0.0 ? 1.0 / st.meanS : st.cfg.mu0;
    const double lam = st.lamHat;
    const int    c   = st.c;
    const size_t Q   = s.qlen;

    // ── c_max 복구 ─────────────────────────────────────────────────
    const int cMaxHw = std::max(st.cfg.cMin, st.cfg.cMax);
    if (st.cMaxEff < cMaxHw && st.cMaxCutAt >= 0.0 && now - st.cMaxCutAt >= p_.cMaxRestoreSec) {
        st.cMaxEff   = cMaxHw;
        st.cMaxCutAt = -1.0;
        Log::info("[Controller] " + pool.name() + " c_max 복구 → " + std::to_string(cMaxHw));
    }

    // ── 증설 효과 확인 ──────────────────────────────────────────────
    if (st.gainPending) {
        st.gainMinQ = std::min(st.gainMinQ, Q);
        if (now - st.gainT0 >= st.gainWin) {
            const double x1   = static_cast<double>(s.done - st.gainDone0) / (now - st.gainT0);
            const double gain = x1 - st.gainC0 * st.gainMu;
            // 창 동안 큐가 비지 않았을 때(모든 스레드에 일이 있었을 때)만 판정
            if (st.gainMinQ >= 1 &&
                gain < p_.gainMin * st.gainMu * (st.gainC1 - st.gainC0)) {
                st.cMaxEff   = std::max(st.cfg.cMin, st.gainC0);
                st.cMaxCutAt = now;
                Log::info("[Controller] " + pool.name() + " 증설 효과 부족 (gain="
                          + std::to_string(static_cast<int>(gain)) + "/s, 기준 "
                          + std::to_string(static_cast<int>(
                                  p_.gainMin * st.gainMu * (st.gainC1 - st.gainC0)))
                          + "/s) → c_max=" + std::to_string(st.cMaxEff));
            }
            st.gainPending = false;
        }
    }

    // ── 결정 ───────────────────────────────────────────────────────
    Decision d;
    d.lam  = lam;
    d.mu   = mu;
    d.cs2  = st.cs2;
    d.sHat = st.sHat;
    d.q    = Q;
    d.l    = c * mu * p_.waitTargetSec;

    const int cmax = st.cMaxEff;
    d.cSs = qmath::minServers(lam, mu, p_.waitTargetSec, p_.waitTailP, p_.rhoMax, p_.ca2,
                              st.cs2, cmax);
    // ŝ 의 무작위 변동: Var(틱 순간값) = (도착률 + 처리율)/Δt, 지수 평활 후 × α/(2−α)
    const double thru = std::min(lam, c * mu);
    d.sSigma = std::sqrt(p_.slopeAlpha / (2.0 - p_.slopeAlpha) * (lam + thru) / p_.tickSec);
    const double sDet  = std::fabs(st.sHat) > p_.slopeZ * d.sSigma ? st.sHat : 0.0;
    const double qPred = static_cast<double>(Q) + sDet * p_.horizonSec;
    if (qPred > d.l) {
        const double need = sDet + (qPred - d.l) / p_.drainSec;   // 초당 더 처리해야 할 양
        const double add  = need > 0.0 ? std::min(need / mu, 1e6) : 0.0;
        d.cB = c + static_cast<int>(std::ceil(add));
    }

    auto capFor = [&](int cc) -> size_t {
        return st.cfg.dynamicCap ? clampCap(cc * mu * p_.waitMaxSec, p_) : st.cfg.fixedCap;
    };

    int want = std::clamp(std::max(d.cSs, d.cB), st.cfg.cMin, cmax);
    while (want < cmax) {
        const double rho = lam / (want * mu);
        if (qmath::kFloor(rho, p_.ca2, st.cs2, p_.kFloorEps) > static_cast<double>(capFor(want)))
            ++want;   // 이 c 로는 1초 안에 무작위 변동을 흡수할 수 없음
        else
            break;
    }
    d.want = want;

    int newC = c;
    if (st.cfg.adaptive) {
        if (want > c) {
            newC          = want;
            st.shrinkFrom = -1.0;
            if (!st.gainPending) {
                st.gainC0 = c;
                st.gainMu = mu;
            }
            st.gainPending = true;
            st.gainC1      = newC;
            st.gainT0      = now;
            {
                const double dc = static_cast<double>(st.gainC1 - st.gainC0);
                const double win = p_.gainZ * p_.gainZ * st.gainC1
                                   / (p_.gainMin * p_.gainMin * dc * dc * std::max(st.gainMu, 1e-9));
                st.gainWin = std::clamp(win, p_.gainWindowSec, 5.0);
            }
            st.gainDone0   = s.done;
            st.gainMinQ    = std::numeric_limits<size_t>::max();
        } else if (want < c) {
            if (static_cast<double>(Q) < d.l / 2.0) {
                if (st.shrinkFrom < 0.0) {
                    st.shrinkFrom = now;
                } else if (now - st.shrinkFrom >= p_.shrinkHoldSec) {
                    newC           = c - 1;
                    st.shrinkFrom  = now;
                    st.gainPending = false;
                }
            } else {
                st.shrinkFrom = -1.0;
            }
        } else {
            st.shrinkFrom = -1.0;
        }
        newC = std::clamp(newC, st.cfg.cMin, st.cMaxEff);
        if (newC != c) {
            pool.setActive(newC);
            st.c = pool.active();
            Log::debug("[Controller] " + pool.name() + " c " + std::to_string(c) + " → "
                       + std::to_string(st.c));
        }
    }

    d.k = capFor(st.c);
    pool.setCap(d.k);

    d.rho    = (mu > 0.0) ? lam / (st.c * mu) : 0.0;
    d.kFloor = std::min(qmath::kFloor(d.rho, p_.ca2, st.cs2, p_.kFloorEps), 1e12);

    const double meanWait = dDone ? dWait / static_cast<double>(dDone) * 1e3 : 0.0;
    const double meanSvc  = dDone ? dSvc / static_cast<double>(dDone) * 1e6 : 0.0;
    writeRow(st, s, d, now, dOff, dShed, dDone, dExp, meanWait, meanSvc);
}

void PoolController::writeRow(const State& st, const PoolSnapshot& s, const Decision& d,
                              double now, uint64_t dOff, uint64_t dShed, uint64_t dDone,
                              uint64_t dExp, double meanWait, double meanSvc) {
    if (!csv_) return;
    std::fprintf(csv_,
                 "%.3f,%s,%zu,%zu,%.1f,%d,%d,%d,%d,%d,%.1f,%.1f,%.3f,%.1f,%.1f,%.4f,%.1f,"
                 "%llu,%llu,%llu,%llu,%.3f,%.1f,%zu,%.3f,%.4f,%.4f,%.4f\n",
                 now, st.cfg.pool->name().c_str(), d.q, d.k, d.l, st.c, d.want, d.cSs, d.cB,
                 st.cMaxEff, d.lam, d.mu, d.cs2, d.sHat, d.sSigma, d.rho, d.kFloor,
                 static_cast<unsigned long long>(dOff), static_cast<unsigned long long>(dShed),
                 static_cast<unsigned long long>(dDone), static_cast<unsigned long long>(dExp),
                 meanWait, meanSvc, s.chunks, epochSec_, st.cfg.pool->cpuSeconds(),
                 reactorCpuSec_, procCpuSec_);
}
