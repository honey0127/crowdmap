#pragma once
#include "ExternalCongestionClient.h"
#include "CongestionCalculator.h"
#include "CacheManager.h"
#include "Logger.h"
#include <vector>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <string>

class CongestionRouter {
public:
    // 외부 API 클라이언트들을 우선순위 순서로 등록
    void addClient(std::shared_ptr<ExternalCongestionClient> client) {
        clients.push_back(client);
    }

    // lat/lng → 혼잡도 결과
    // 우선순위: 캐시 → 외부 API → 내부 계산 fallback
    //
    // [캐시 스탬피드 방지] 인기 존의 캐시가 만료되는 순간 수천 조회가 동시에
    // miss를 내면, 워커들이 같은 외부 API를 한꺼번에 두드린다(stampede).
    //  - single-flight: 존당 한 스레드만 외부 API 갱신을 수행
    //  - stale-while-revalidate: 갱신 중인 다른 스레드는 만료된 값을 즉시 응답
    //  - negative cache: 실패(fallback) 결과는 짧은 TTL(10초)로만 캐싱
    CongestionResult resolve(double lat, double lng,
                             int internalUserCount,
                             CacheManager& cache,
                             int zoneId) {
        // 1. 캐시 먼저 확인
        CongestionResult cached;
        if (cache.get(zoneId, cached)) {
            Log::debug("[Router] Cache hit zone=" + std::to_string(zoneId));
            return cached;
        }

        // 2. single-flight 선점: 이 존의 외부 API 갱신 권한을 한 스레드만 가진다
        bool claimed;
        {
            std::lock_guard<std::mutex> lock(inflightMutex);
            claimed = inflight.insert(zoneId).second;
        }
        if (!claimed) {
            // 다른 워커가 갱신 중: 낡은 값이라도 즉시 응답 (외부 API 중복 호출 차단)
            if (cache.getStale(zoneId, cached, STALE_MAX_AGE_SEC)) {
                Log::debug("[Router] Stale hit zone=" + std::to_string(zoneId));
                return cached;
            }
            // stale조차 없으면 내부 계산으로 즉시 응답. 갱신 중인 스레드가
            // 곧 캐시를 채울 것이므로 이 결과는 캐싱하지 않는다.
            return internalCalc.calculateCongestion(internalUserCount);
        }

        // 선점 성공: 어떤 경로로 빠져나가든 inflight 표식을 반드시 해제
        struct InflightGuard {
            CongestionRouter* router;
            int zone;
            ~InflightGuard() {
                std::lock_guard<std::mutex> lock(router->inflightMutex);
                router->inflight.erase(zone);
            }
        } guard{this, zoneId};

        // double-check: 선점 직전에 다른 스레드가 갱신을 끝냈을 수 있다
        if (cache.get(zoneId, cached)) return cached;

        // 3. 외부 API 순서대로 시도
        for (auto& client : clients) {
            if (!client->covers(lat, lng)) continue;

            auto ext = client->getCongestion(lat, lng);
            if (!ext.valid) continue;

            Log::debug("[Router] External hit source=" + ext.source);
            CongestionResult r = externalToResult(ext);
            cache.set(zoneId, r);
            return r;
        }

        // 4. Fallback: 내부 사용자 수 기반 계산.
        // 기본 TTL(30초)로 캐싱하면 외부 API 복구가 30초씩 가려지고,
        // 캐싱하지 않으면 다음 조회가 곧바로 또 외부 API를 두드린다.
        // → 짧은 TTL의 negative cache가 둘 사이의 균형점.
        Log::debug("[Router] Fallback internal count=" + std::to_string(internalUserCount));
        CongestionResult r = internalCalc.calculateCongestion(internalUserCount);
        cache.set(zoneId, r, FAILURE_TTL_SEC);
        return r;
    }

private:
    static constexpr int STALE_MAX_AGE_SEC = 300; // stale 응답 허용 최대 나이
    static constexpr int FAILURE_TTL_SEC   = 10;  // 실패 결과의 짧은 TTL

    std::vector<std::shared_ptr<ExternalCongestionClient>> clients;
    CongestionCalculator internalCalc;

    std::mutex inflightMutex;
    std::unordered_set<int> inflight; // 외부 API 갱신이 진행 중인 zoneId 집합

    CongestionResult externalToResult(const ExternalCongestionResult& ext) {
        CongestionLevel lvl;
        switch (ext.level) {
            case 1:  lvl = CongestionLevel::RELAXED;  break;
            case 2:  lvl = CongestionLevel::MODERATE; break;
            default: lvl = CongestionLevel::CROWDED;  break;
        }
        return {lvl, ext.level / 4.0, 0};
    }
};
