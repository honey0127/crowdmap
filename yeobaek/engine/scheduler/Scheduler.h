#pragma once
#include "../congestion/ForecastProvider.h"
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────────
// [신규] Scheduler — 시간의존 소규모 TSP (절차서 결정 D, Phase 2-3)
//
// "방문 순서를 정하면 각 장소 도착 시각이 정해지고, 그 시각의 예보로 혼잡도가
//  평가되는" 시간의존 문제. stop 이 작으므로(≤8) 완전탐색, 그 이상은 2-opt.
//
//   arrival[0]    = startTime
//   arrival[i]    = leave[i-1] + travel(i-1 → i)     ( leave = arrival + dwell )
//   congestion[i] = ForecastProvider.get(area[i], arrival[i]).level / 4
//
// 비용(부록 C):
//   cost = w_c·Σ c_i  +  w_d·(총이동거리/D_max)  +  w_s·Σ (1−sim_j)·[치환된 stop]
//
// 고혼잡 stop 은 감성 쌍둥이(twins)로 치환 가능하되, 유사도가 낮으면 w_s 페널티가
// 억제한다 → 아무거나 바꾸지 않는다.
// ─────────────────────────────────────────────────────────────────────────────

struct TwinCandidate {
    long contentId = 0;
    double lat = 0, lng = 0;
    std::string areaName;   // 예보지점 (place_area_map)
    double similarity = 0;  // 원 stop 과의 감성 유사도 0~1
};

struct SchedStop {
    long contentId = 0;
    double lat = 0, lng = 0;
    std::string areaName;
    int dwellSec = 3600;                // 카테고리 기본 체류(초)
    std::vector<TwinCandidate> twins;   // 비었으면 치환 불가
};

struct SchedWeights { double congestion = 1.0, distance = 0.3, similarity = 0.5; };

struct PlanStop {
    long contentId = 0;
    long substitutedFrom = -1;          // 치환 시 원본 id, 아니면 -1
    std::string areaName;
    long arrivalUnix = 0;
    int forecastLevel = 1;              // 1~4
    double lat = 0, lng = 0;
    double similarity = 1.0;            // 치환된 경우 쌍둥이 유사도, 아니면 1.0
};

struct Plan {
    std::vector<PlanStop> ordered;
    double totalCost = 0;
    int savedCongestionPct = 0;
    bool valid = false;
};

class Scheduler {
public:
    explicit Scheduler(ForecastProvider* provider,
                       double avgSpeedKmh = 25.0,   // 도심 평균속도 근사
                       double dMaxKm = 50.0,        // 이동거리 정규화 상수
                       int highCongestionLevel = 3) // 이 레벨 이상일 때만 치환 고려
        : provider(provider), speedKmh(avgSpeedKmh), dMaxKm(dMaxKm),
          highLevel(highCongestionLevel) {}

    // keepOrder=true 면 재정렬하지 않고 "사용자가 고른 순서 그대로" 평가한다.
    //   → 사용자가 짠 플랜대로 가되, 각 stop 의 도착시점 예보만 채워준다.
    //   (allowSubstitution 이 함께 true 면 고혼잡 stop 만 감성 쌍둥이로 스왑)
    Plan optimize(const std::vector<SchedStop>& stops, long startUnix,
                  SchedWeights w, bool allowSubstitution, bool keepOrder = false) {
        Plan best;
        const int n = static_cast<int>(stops.size());
        if (n == 0) return best;

        std::vector<int> order(n);
        std::iota(order.begin(), order.end(), 0);

        double bestCost = std::numeric_limits<double>::max();
        std::vector<int> bestOrder;
        Plan bestPlan;

        if (keepOrder) {
            // 재정렬 없이 입력 순서 그대로 평가(치환은 옵션).
            bestPlan = evaluate(stops, order, startUnix, w, allowSubstitution);
        } else if (n <= 8) {
            // 완전탐색: 모든 방문 순서
            std::vector<int> perm = order;
            std::sort(perm.begin(), perm.end());
            do {
                Plan p = evaluate(stops, perm, startUnix, w, allowSubstitution);
                if (p.totalCost < bestCost) { bestCost = p.totalCost; bestPlan = p; }
            } while (std::next_permutation(perm.begin(), perm.end()));
        } else {
            // 2-opt 지역탐색
            std::vector<int> cur = order;
            Plan curPlan = evaluate(stops, cur, startUnix, w, allowSubstitution);
            bestCost = curPlan.totalCost; bestPlan = curPlan;
            bool improved = true; int guard = 0;
            while (improved && guard++ < 50) {
                improved = false;
                for (int i = 0; i < n - 1 && !improved; ++i) {
                    for (int k = i + 1; k < n; ++k) {
                        std::vector<int> cand = cur;
                        std::reverse(cand.begin() + i, cand.begin() + k + 1);
                        Plan p = evaluate(stops, cand, startUnix, w, allowSubstitution);
                        if (p.totalCost + 1e-9 < bestCost) {
                            bestCost = p.totalCost; bestPlan = p; cur = cand; improved = true; break;
                        }
                    }
                }
            }
        }

        // 절약 혼잡도: (원래 입력 순서·무치환) 대비 최적안의 혼잡 총합 감소율
        Plan baseline = evaluate(stops, order, startUnix, w, /*allowSubstitution=*/false);
        double baseSum = 0, optSum = 0;
        for (auto& s : baseline.ordered) baseSum += s.forecastLevel;
        for (auto& s : bestPlan.ordered)  optSum  += s.forecastLevel;
        int saved = 0;
        if (baseSum > 0) saved = (int)std::lround((baseSum - optSum) / baseSum * 100.0);
        bestPlan.savedCongestionPct = std::max(0, saved);
        bestPlan.valid = true;
        return bestPlan;
    }

private:
    ForecastProvider* provider;
    double speedKmh, dMaxKm;
    int highLevel = 3;

    static double haversineKm(double lat1, double lng1, double lat2, double lng2) {
        constexpr double R = 6371.0088;
        auto rad = [](double d){ return d * M_PI / 180.0; };
        double dLat = rad(lat2 - lat1), dLng = rad(lng2 - lng1);
        double a = std::sin(dLat/2)*std::sin(dLat/2)
                 + std::cos(rad(lat1))*std::cos(rad(lat2))*std::sin(dLng/2)*std::sin(dLng/2);
        return 2 * R * std::asin(std::min(1.0, std::sqrt(a)));
    }

    int forecastLevel(const std::string& area, long arrivalUnix) {
        if (!provider || area.empty()) return 2; // 예보지점 없으면 중립(보통)
        return provider->get(area, arrivalUnix).level;
    }

    // 한 방문 순서(order)에 대한 계획 + 비용. 각 위치에서 원본/쌍둥이 중 국소 최적 선택.
    Plan evaluate(const std::vector<SchedStop>& stops, const std::vector<int>& order,
                  long startUnix, SchedWeights w, bool allowSubstitution) {
        Plan plan;
        const int n = static_cast<int>(order.size());
        double sumCong = 0, totalDistKm = 0, simPenalty = 0;
        long clock = startUnix;
        double prevLat = 0, prevLng = 0; bool hasPrev = false;
        std::unordered_set<long> usedIds;   // 같은 장소 중복 방문 방지

        for (int p = 0; p < n; ++p) {
            const SchedStop& s = stops[order[p]];

            struct Opt { long id; long from; double lat, lng; std::string area; double sim; };
            std::vector<Opt> opts;
            opts.push_back({s.contentId, -1, s.lat, s.lng, s.areaName, 1.0});

            // 치환은 "원본 stop 의 도착시점 예보가 고혼잡 임계 이상" 일 때만 고려한다(부록 C).
            // 저혼잡/중립에서는 원본을 지켜, 불필요한 스왑·중복 방문을 막는다.
            if (allowSubstitution && !s.twins.empty()) {
                double edge0 = hasPrev ? haversineKm(prevLat, prevLng, s.lat, s.lng) : 0.0;
                long arr0 = hasPrev
                    ? clock + (long)std::llround(edge0 / std::max(1.0, speedKmh) * 3600.0)
                    : startUnix;
                if (forecastLevel(s.areaName, arr0) >= highLevel) {
                    for (auto& t : s.twins) {
                        if (usedIds.count(t.contentId) || t.contentId == s.contentId) continue;
                        opts.push_back({t.contentId, s.contentId, t.lat, t.lng, t.areaName, t.similarity});
                    }
                }
            }

            double bestLocal = std::numeric_limits<double>::max();
            Opt bestOpt = opts[0]; long bestArrival = clock; int bestLevel = 1; double bestEdge = 0;

            for (auto& o : opts) {
                double edgeKm = hasPrev ? haversineKm(prevLat, prevLng, o.lat, o.lng) : 0.0;
                long arrival = hasPrev
                    ? clock + (long)std::llround(edgeKm / std::max(1.0, speedKmh) * 3600.0)
                    : startUnix;
                int level = forecastLevel(o.area, arrival);
                double c = level / 4.0;
                bool substituted = (o.from != -1);
                double local = w.congestion * c
                             + w.distance * (edgeKm / dMaxKm)
                             + (substituted ? w.similarity * (1.0 - o.sim) : 0.0);
                if (local < bestLocal) {
                    bestLocal = local; bestOpt = o; bestArrival = arrival;
                    bestLevel = level; bestEdge = edgeKm;
                }
            }

            PlanStop ps;
            ps.contentId = bestOpt.id;
            ps.substitutedFrom = bestOpt.from;
            ps.areaName = bestOpt.area;
            ps.arrivalUnix = bestArrival;
            ps.forecastLevel = bestLevel;
            ps.lat = bestOpt.lat; ps.lng = bestOpt.lng;
            ps.similarity = bestOpt.sim;
            plan.ordered.push_back(ps);

            sumCong += bestLevel / 4.0;
            totalDistKm += bestEdge;
            if (bestOpt.from != -1) simPenalty += (1.0 - bestOpt.sim);

            usedIds.insert(bestOpt.id);
            clock = bestArrival + s.dwellSec;   // 다음 이동 출발 시각
            prevLat = bestOpt.lat; prevLng = bestOpt.lng; hasPrev = true;
        }

        plan.totalCost = w.congestion * sumCong
                       + w.distance * (totalDistKm / dMaxKm)
                       + w.similarity * simPenalty;
        return plan;
    }
};
