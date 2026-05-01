#pragma once
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <string>
#include "CongestionCalculator.h"

struct CacheEntry {
    CongestionResult result;
    std::chrono::steady_clock::time_point timestamp;
};

class CacheManager {
public:
    CacheManager(int ttlSeconds = 30) : ttl(ttlSeconds) {}

    // 캐시에서 혼잡도 조회
    bool get(int zoneId, CongestionResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(zoneId);
        if (it == cache.end()) return false;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.timestamp).count();

        if (elapsed > ttl) {
            cache.erase(it);  // TTL 만료 시 제거
            return false;
        }
        result = it->second.result;
        return true;
    }

    // 캐시에 혼잡도 저장
    void set(int zoneId, const CongestionResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        cache[zoneId] = {result, std::chrono::steady_clock::now()};
    }

    // 캐시 통계
    int size() {
        std::lock_guard<std::mutex> lock(mutex);
        return cache.size();
    }

private:
    std::unordered_map<int, CacheEntry> cache;
    std::mutex mutex;
    int ttl;
};
