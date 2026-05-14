#pragma once
#include <unordered_map>
#include <list>
#include <chrono>
#include <mutex>
#include "CongestionCalculator.h"

struct CacheEntry {
    CongestionResult result;
    std::chrono::steady_clock::time_point timestamp;
};

// LRU 캐시: TTL 만료 + maxSize 초과 시 가장 오래된 항목 제거
class CacheManager {
public:
    explicit CacheManager(int ttlSeconds = 30, int maxSize = 1000)
        : ttl(ttlSeconds), maxSize(maxSize) {}

    bool get(int zoneId, CongestionResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(zoneId);
        if (it == cache.end()) return false;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.first.timestamp).count();

        if (elapsed > ttl) {
            lruOrder.erase(it->second.second);
            cache.erase(it);
            return false;
        }

        // 접근 시 LRU 순서 갱신 (front = 가장 최근)
        lruOrder.erase(it->second.second);
        lruOrder.push_front(zoneId);
        it->second.second = lruOrder.begin();

        result = it->second.first.result;
        return true;
    }

    void set(int zoneId, const CongestionResult& result) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(zoneId);
        if (it != cache.end()) {
            lruOrder.erase(it->second.second);
            lruOrder.push_front(zoneId);
            it->second.first = {result, std::chrono::steady_clock::now()};
            it->second.second = lruOrder.begin();
        } else {
            evictIfNeeded();
            lruOrder.push_front(zoneId);
            cache[zoneId] = {{result, std::chrono::steady_clock::now()}, lruOrder.begin()};
        }
    }

    int size() {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(cache.size());
    }

private:
    int ttl;
    int maxSize;
    std::list<int> lruOrder; // front = 가장 최근, back = 가장 오래됨
    std::unordered_map<int, std::pair<CacheEntry, std::list<int>::iterator>> cache;
    std::mutex mutex;

    void evictIfNeeded() {
        while ((int)cache.size() >= maxSize) {
            int oldest = lruOrder.back();
            lruOrder.pop_back();
            cache.erase(oldest);
        }
    }
};
