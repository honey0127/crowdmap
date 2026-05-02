#pragma once
#include "ExternalCongestionClient.h"
#include "CongestionCalculator.h"
#include "CacheManager.h"
#include <vector>
#include <memory>
#include <iostream>

class CongestionRouter {
public:
    // 외부 API 클라이언트들을 우선순위 순서로 등록
    void addClient(std::shared_ptr<ExternalCongestionClient> client) {
        clients.push_back(client);
    }

    // lat/lng → 혼잡도 결과
    // 우선순위: 외부 API → 내부 계산 fallback
    CongestionResult resolve(double lat, double lng,
                             int internalUserCount,
                             CacheManager& cache,
                             int zoneId) {
        // 1. 캐시 먼저 확인
        CongestionResult cached;
        if (cache.get(zoneId, cached)) {
            std::cout << "[Router] Cache hit zone=" << zoneId << "\n";
            return cached;
        }

        // 2. 외부 API 순서대로 시도
        for (auto& client : clients) {
            if (!client->covers(lat, lng)) continue;

            auto ext = client->getCongestion(lat, lng);
            if (!ext.valid) continue;

            std::cout << "[Router] External hit source=" << ext.source << "\n";
            CongestionResult r = externalToResult(ext);
            cache.set(zoneId, r);
            return r;
        }

        // 3. Fallback: 내부 사용자 수 기반 계산
        std::cout << "[Router] Fallback internal count=" << internalUserCount << "\n";
        CongestionResult r = internalCalc.calculateCongestion(internalUserCount);
        cache.set(zoneId, r);
        return r;
    }

private:
    std::vector<std::shared_ptr<ExternalCongestionClient>> clients;
    CongestionCalculator internalCalc;

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