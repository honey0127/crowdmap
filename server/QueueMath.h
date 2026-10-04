#pragma once
// 대기행렬 계산 (PoolController 와 단위 테스트가 함께 쓴다).
//
// 빌드가 -ffast-math 라 inf/NaN 판정(std::isinf 등)이 최적화로 사라질 수 있다.
// 그래서 포화(ρ ≥ 1)는 무한대 대신 BIG 상수와 명시적 분기로 표현한다.
#include <cmath>

namespace qmath {

constexpr double BIG = 1e18;

// M/M/c 대기 확률 C(c, a). a = λ/μ (제시 부하). a ≥ c 이면 1.
inline double erlangC(int c, double a) {
    if (c <= 0) return 1.0;
    if (a <= 0.0) return 0.0;
    if (a >= c) return 1.0;
    double b = 1.0;                                   // Erlang B 재귀 (수치 안정)
    for (int k = 1; k <= c; ++k) b = a * b / (k + a * b);
    const double rho = a / c;
    return b / (1.0 - rho + rho * b);
}

// 대기시간 꼬리 P(Wq > t).
// M/M/c 정확식 C(c,a)·exp(−(cμ−λ)t) 에 변동 보정을 더한 근사:
// 조건부 대기의 평균을 (Ca²+Cs²)/2 배로 늘린다(Kingman·Allen–Cunneen 과 같은 보정).
// Ca² = Cs² = 1 이면 M/M/c 정확식과 같다.
inline double pWaitGt(int c, double lam, double mu, double t, double ca2, double cs2) {
    if (lam <= 0.0) return 0.0;
    if (mu <= 0.0 || c <= 0) return 1.0;
    const double a = lam / mu;
    if (a >= c) return 1.0;
    double v = (ca2 + cs2) / 2.0;
    if (v < 1e-3) v = 1e-3;                           // 결정적 도착·서비스(v=0)에서 0 나눗셈 방지
    return erlangC(c, a) * std::exp(-(c * mu - lam) * t / v);
}

// ρ ≤ rhoMax 이고 P(Wq > t) ≤ p 를 만족하는 최소 서버 수 (1..cMax). 없으면 cMax.
inline int minServers(double lam, double mu, double t, double p, double rhoMax,
                      double ca2, double cs2, int cMax) {
    if (cMax < 1) cMax = 1;
    if (lam <= 0.0) return 1;
    if (mu <= 0.0) return cMax;
    for (int c = 1; c <= cMax; ++c) {
        const double rho = lam / (c * mu);
        if (rho > rhoMax) continue;
        if (pWaitGt(c, lam, mu, t, ca2, cs2) <= p) return c;
    }
    return cMax;
}

// 무작위 변동 때문에 버리는 일이 없게 하는 최소 큐 크기 (일반 분포 근사).
// P(Lq ≥ K) ≈ ε → K ≈ ρ(Ca²+Cs²)/(2(1−ρ))·ln(ρ/ε). ρ ≥ 0.999 이면 BIG(포화).
inline double kFloor(double rho, double ca2, double cs2, double eps) {
    if (rho <= eps) return 0.0;
    if (rho >= 0.999) return BIG;
    return rho * (ca2 + cs2) / (2.0 * (1.0 - rho)) * std::log(rho / eps);
}

// 같은 값의 M/M/c 정확식: P(Lq ≥ K) = C(c,a)·ρ^K ≤ ε 인 최소 K. 근사식 검증용.
inline double kFloorMMc(int c, double a, double eps) {
    const double rho = a / c;
    if (rho >= 0.999) return BIG;
    const double pw = erlangC(c, a);
    if (pw <= eps) return 0.0;
    return std::ceil(std::log(eps / pw) / std::log(rho));
}

}  // namespace qmath
