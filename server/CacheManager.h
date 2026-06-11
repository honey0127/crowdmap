#pragma once
#include <unordered_map>
#include <list>
#include <chrono>
#include <mutex>
#include "CongestionCalculator.h"

struct CacheEntry {
    CongestionResult result;
    std::chrono::steady_clock::time_point timestamp;
    int ttlSec;   // 항목별 TTL: 성공은 길게, 실패(fallback)는 짧게 캐싱하기 위함
};

// LRU 캐시: TTL 만료 + maxSize 초과 시 가장 오래된 항목 제거
//
// [대규모 트래픽 대응] TTL이 지난 항목도 즉시 지우지 않고 보관한다.
// 외부 API가 느리거나 워커가 포화일 때 "조금 낡은 값"을 즉시 응답하는
// stale-while-revalidate 전략의 재료가 되기 때문이다(getStale 참조).
// 메모리는 maxSize LRU가 상한을 보장한다.
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

        // 만료: miss로 처리하되 항목은 남겨둔다 (stale 응답용 보존)
        if (elapsed > it->second.first.ttlSec) return false;

        // 접근 시 LRU 순서 갱신 (front = 가장 최근)
        lruOrder.erase(it->second.second);
        lruOrder.push_front(zoneId);
        it->second.second = lruOrder.begin();

        result = it->second.first.result;
        return true;
    }

    // TTL이 지났더라도 maxAgeSec 이내의 항목이면 반환한다.
    // 다른 스레드가 같은 존을 갱신 중이거나(single-flight 대기측)
    // 서버가 포화 상태(load shedding)일 때 즉시 응답하는 용도.
    // LRU는 건드리지 않는다 — stale 접근이 fresh 항목을 밀어내지 않도록.
    bool getStale(int zoneId, CongestionResult& result, int maxAgeSec) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(zoneId);
        if (it == cache.end()) return false;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second.first.timestamp).count();
        if (elapsed > maxAgeSec) return false;

        result = it->second.first.result;
        return true;
    }

    // ttlSec < 0 이면 생성자의 기본 TTL 사용.
    // 외부 API 실패로 만든 fallback 결과는 짧은 TTL로 넣어(negative cache)
    // 실패가 오래 가려지는 것과 즉시 재시도 폭주를 동시에 막는다.
    void set(int zoneId, const CongestionResult& result, int ttlSec = -1) {
        const int effectiveTtl = (ttlSec < 0) ? ttl : ttlSec;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(zoneId);
        if (it != cache.end()) {
            lruOrder.erase(it->second.second);
            lruOrder.push_front(zoneId);
            it->second.first = {result, std::chrono::steady_clock::now(), effectiveTtl};
            it->second.second = lruOrder.begin();
        } else {
            evictIfNeeded();
            lruOrder.push_front(zoneId);
            cache[zoneId] = {{result, std::chrono::steady_clock::now(), effectiveTtl},
                             lruOrder.begin()};
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
