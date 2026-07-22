#pragma once
#include "../external/SeoulCityDataForecastClient.h"
#include <unordered_map>
#include <list>
#include <chrono>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// [신규] ForecastProvider — 예측 경로 전용 얇은 조회+캐시 계층 (절차서 결정 B, Phase 2-2)
//
// 실시간 경로(CongestionRouter, single-flight/negative-cache)와 분리한다.
// 캐시 키 = (seoulAreaName, timeBucket).  timeBucket = 도착시각을 예보 간격(1시간)
// 으로 라운딩 → 같은 장소·같은 시간버킷 조회는 캐시 히트.
//
// CacheManager 의 LRU+TTL 설계를 그대로 따르되(성공은 길게, fallback 은 짧게),
// 키 타입이 (area,bucket) 문자열이라 전용 캐시로 구현한다.
//
// fallback 순서(절차서 2-1 규칙): 예보(self) → 인근 장소 예보 → 실시간(self) → 중립값.
// ─────────────────────────────────────────────────────────────────────────────
class ForecastProvider {
public:
    explicit ForecastProvider(std::shared_ptr<SeoulCityDataForecastClient> client,
                              int bucketSeconds = 3600,
                              int okTtlSec = 600,       // 예보 성공값 TTL (길게)
                              int fallbackTtlSec = 60,  // 실시간 fallback TTL (짧게)
                              int maxSize = 4096)
        : client(std::move(client)), bucketSec(bucketSeconds),
          okTtl(okTtlSec), fbTtl(fallbackTtlSec), maxSize(maxSize) {}

    virtual ~ForecastProvider() = default;

    // area 예보지점 이름 + 도착 epoch → ForecastResult (level 1~4).
    // virtual: 오프라인 테스트가 결정적 예보를 주입할 수 있도록(프로덕션 경로는 아래 구현).
    virtual ForecastResult get(const std::string& areaName, long arrivalUnixTime) {
        const long bucket = (bucketSec > 0) ? (arrivalUnixTime / bucketSec) * bucketSec
                                            : arrivalUnixTime;
        const std::string key = areaName + "|" + std::to_string(bucket);

        ForecastResult cached;
        if (cacheGet(key, cached)) return cached;

        if (!client) return {2, 0, 0, 0, false};

        // 1) self 예보
        ForecastResult r = client->getForecastByArea(areaName, arrivalUnixTime);
        if (r.valid && r.fcstUnixTime > 0) { cacheSet(key, r, okTtl); return r; }

        // 2) 인근 장소 예보 (self 가 실시간 fallback 이거나 invalid 일 때)
        for (const std::string& nb : nearestNeighbors(areaName, 2)) {
            ForecastResult rn = client->getForecastByArea(nb, arrivalUnixTime);
            if (rn.valid && rn.fcstUnixTime > 0) { cacheSet(key, rn, okTtl); return rn; }
        }

        // 3) self 실시간 fallback (1단계에서 얻어둔 값이 있으면 그것)
        if (r.valid) { cacheSet(key, r, fbTtl); return r; }

        // 4) 중립값 (보통) — 데모 안정성. 캐시하지 않음(다음 조회에 재시도).
        return {2, 0, 0, 0, false};
    }

    // 정규화 ratio (0~1) = level/4. pybind forecast.get 이 함께 반환.
    static double toRatio(int level) { return level / 4.0; }

private:
    std::shared_ptr<SeoulCityDataForecastClient> client;
    int bucketSec, okTtl, fbTtl, maxSize;

    struct Entry { ForecastResult result; std::chrono::steady_clock::time_point ts; int ttl; };
    std::list<std::string> lru;                                   // front = 최근
    std::unordered_map<std::string, std::pair<Entry, std::list<std::string>::iterator>> cache;
    std::mutex mtx;

    // 인근 예보지점: areaName 좌표 기준 최근접 k개 이름.
    std::vector<std::string> nearestNeighbors(const std::string& areaName, int k) {
        std::vector<std::string> out;
        double lat, lng;
        if (!client->areaCoordinate(areaName, lat, lng)) return out;
        std::vector<std::pair<double, std::string>> cand;
        for (auto& [n, c] : client->getAreas()) {
            if (n == areaName) continue;
            double dLat = lat - c.first, dLng = lng - c.second;
            cand.emplace_back(dLat*dLat + dLng*dLng, n);
        }
        std::partial_sort(cand.begin(),
                          cand.begin() + std::min<size_t>(k, cand.size()),
                          cand.end());
        for (int i = 0; i < k && i < (int)cand.size(); ++i) out.push_back(cand[i].second);
        return out;
    }

    bool cacheGet(const std::string& key, ForecastResult& out) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = cache.find(key);
        if (it == cache.end()) return false;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second.first.ts).count();
        if (elapsed > it->second.first.ttl) return false;   // 만료 → miss (항목은 유지)
        lru.erase(it->second.second);
        lru.push_front(key);
        it->second.second = lru.begin();
        out = it->second.first.result;
        return true;
    }

    void cacheSet(const std::string& key, const ForecastResult& r, int ttl) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = cache.find(key);
        if (it != cache.end()) {
            lru.erase(it->second.second);
            lru.push_front(key);
            it->second.first = {r, std::chrono::steady_clock::now(), ttl};
            it->second.second = lru.begin();
            return;
        }
        while ((int)cache.size() >= maxSize && !lru.empty()) {
            cache.erase(lru.back());
            lru.pop_back();
        }
        lru.push_front(key);
        cache[key] = {{r, std::chrono::steady_clock::now(), ttl}, lru.begin()};
    }
};
