#ifndef SPATIAL_DENSITY_ENGINE_H
#define SPATIAL_DENSITY_ENGINE_H

#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include "SpatialHash.h"
#include "GridZone.h"
#include "RedisClient.h"

// 기기들이 P2P 집계 후 서버에 올리는 존 단위 밀도 리포트
struct ZoneDensityReport {
    int count;
    std::chrono::steady_clock::time_point timestamp;
};

class SpatialDensityEngine {
private:
    struct Bucket {
        std::shared_mutex mutex;
        std::unordered_map<int64_t, GridZone> zones;
    };

    static constexpr size_t NUM_STRIPES = 1024;
    std::vector<std::unique_ptr<Bucket>> m_buckets;

    // 존 밀도 리포트 저장소 (zoneId → 최신 리포트)
    std::unordered_map<int, ZoneDensityReport> m_zoneReports;
    std::shared_mutex m_zoneReportsMutex;

    // ── Redis 영속성 (선택적) ─────────────────────────────────────────
    // 서버 재시작 시 5분 윈도우 데이터가 증발하지 않도록 이벤트를 Redis에
    // 미러링한다. Redis 부재 시 nullptr → 순수 메모리 모드로 동작.
    //
    // recordLocation은 리액터(이벤트 루프) 스레드에서 호출되므로, 호출마다
    // Redis 왕복(블로킹)을 하면 이벤트 루프 전체가 Redis 지연에 직렬화된다.
    // → 이벤트를 큐에 넣고 전용 writer 스레드가 비동기로 저장한다.
    //   영속성은 best-effort: 큐 포화 시 폐기하고 실시간 경로를 지킨다.
    struct PendingEvent {
        int64_t   key;
        int32_t   userId;
        long long unixTs;
    };
    RedisClient*              m_redis = nullptr;
    std::deque<PendingEvent>  m_redisQueue;
    std::mutex                m_redisQueueMutex;
    std::condition_variable   m_redisCv;
    std::thread               m_redisWriter;
    std::atomic<bool>         m_redisWriterRunning{false};
    static constexpr size_t   REDIS_QUEUE_MAX = 10'000;

public:
    SpatialDensityEngine() {
        m_buckets.reserve(NUM_STRIPES);
        for (size_t i = 0; i < NUM_STRIPES; ++i) {
            m_buckets.push_back(std::make_unique<Bucket>());
        }
    }

    ~SpatialDensityEngine() {
        stopRedisPersistence();
    }

    // ── Redis 영속성 연결 ──
    // redis 포인터의 수명은 호출자(main)가 보장해야 하며, RedisClient가
    // 소멸하기 전에 반드시 stopRedisPersistence()가 호출되어야 한다.
    void setRedis(RedisClient* redis) {
        m_redis = redis;
        if (m_redis && !m_redisWriterRunning.exchange(true)) {
            m_redisWriter = std::thread([this] { redisWriterLoop(); });
        }
    }

    // Redis writer 스레드 종료 (idempotent). main의 명시적 종료 경로와
    // 소멸자 양쪽에서 호출돼도 안전하다.
    void stopRedisPersistence() {
        if (m_redisWriterRunning.exchange(false)) {
            m_redisCv.notify_all();
            if (m_redisWriter.joinable()) m_redisWriter.join();
        }
        m_redis = nullptr;
    }

    // 서버 시작 시 Redis에 남은 이벤트를 원본 타임스탬프로 복원한다.
    // unix 시각(벽시계) → steady_clock 시각으로 나이(age)만큼 되돌려 환산.
    // 5분(GridZone 윈도우)을 넘긴 이벤트는 버린다.
    size_t restoreFromRedis(RedisClient& redis) {
        const auto events = redis.loadAllEvents();
        const auto steadyNow = std::chrono::steady_clock::now();
        const long long sysNowSec =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

        size_t restored = 0;
        for (const auto& [key, userId, unixTs] : events) {
            const long long ageSec = sysNowSec - unixTs;
            if (ageSec < 0 || ageSec > 300) continue;  // 만료 또는 시계 역행

            const auto originalTime = steadyNow - std::chrono::seconds(ageSec);
            auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            bucket->zones[key].injectHistorical(userId, originalTime);
            ++restored;
        }
        return restored;
    }

    // bleCount > 0 이면 BLE 감지 기기 수도 함께 기록 (기본값 0 = BLE 없음)
    void recordLocation(int32_t userId, double lat, double lon, int bleCount = 0) {
        int64_t key = SpatialHash::generateKey(lat, lon);
        auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];
        auto now = std::chrono::steady_clock::now();

        {
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            bucket->zones[key].update(userId, now);
            if (bleCount > 0) {
                bucket->zones[key].updateBle(bleCount, now);
            }
        }

        // Redis 미러링: 리액터 스레드를 막지 않도록 큐에만 넣는다
        if (m_redis && m_redisWriterRunning.load(std::memory_order_relaxed)) {
            const long long unixTs =
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            std::lock_guard<std::mutex> qlock(m_redisQueueMutex);
            if (m_redisQueue.size() < REDIS_QUEUE_MAX) {
                m_redisQueue.push_back({key, userId, unixTs});
                m_redisCv.notify_one();
            }
            // 포화 시 조용히 폐기: 영속성은 best-effort
        }
    }

    // GPS 기반 밀도와 BLE 감지 수를 가중 평균으로 보정하여 반환
    // BLE 데이터가 없으면 GPS 밀도만 반환
    size_t getDensity(double lat, double lon) {
        size_t totalGps = 0;
        int    maxBle   = 0;

        for (double dLat = -SpatialHash::GRID_SIZE; dLat <= SpatialHash::GRID_SIZE; dLat += SpatialHash::GRID_SIZE) {
            for (double dLon = -SpatialHash::GRID_SIZE; dLon <= SpatialHash::GRID_SIZE; dLon += SpatialHash::GRID_SIZE) {

                int64_t key = SpatialHash::generateKey(lat + dLat, lon + dLon);
                auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];

                {
                    std::shared_lock<std::shared_mutex> lock(bucket->mutex);
                    auto it = bucket->zones.find(key);
                    if (it != bucket->zones.end()) {
                        totalGps += it->second.getDensity();
                        int zoneBle = it->second.getMaxBleCount();
                        if (zoneBle > maxBle) maxBle = zoneBle;
                    }
                }
            }
        }

        if (maxBle > 0) {
            // GPS 카운트(60%)와 BLE 감지 수(40%) 가중 평균으로 보정
            return static_cast<size_t>(0.6 * static_cast<double>(totalGps)
                                     + 0.4 * static_cast<double>(maxBle));
        }
        return totalGps;
    }

    // P2P 집계 리포트 기록 (zoneId 단위, 5분 TTL)
    void recordZoneDensity(int zoneId, int count) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock<std::shared_mutex> lock(m_zoneReportsMutex);
        m_zoneReports[zoneId] = {count, now};
    }

    // 유효한(5분 이내) 존 밀도 리포트 반환. 없거나 만료되면 -1 반환
    int getZoneReport(int zoneId) {
        std::shared_lock<std::shared_mutex> lock(m_zoneReportsMutex);
        auto it = m_zoneReports.find(zoneId);
        if (it == m_zoneReports.end()) return -1;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second.timestamp).count();
        return (elapsed <= 300) ? it->second.count : -1;
    }

    void globalCleanup() {
        auto now = std::chrono::steady_clock::now();
        for (auto& bucket : m_buckets) {
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            for (auto it = bucket->zones.begin(); it != bucket->zones.end(); ) {
                it->second.cleanup(now);
                it->second.cleanupBle(now);
                if (it->second.getDensity() == 0 && it->second.getMaxBleCount() == 0) {
                    it = bucket->zones.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // 만료된 존 밀도 리포트 정리
        std::unique_lock<std::shared_mutex> zrLock(m_zoneReportsMutex);
        for (auto it = m_zoneReports.begin(); it != m_zoneReports.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.timestamp).count();
            it = (elapsed > 300) ? m_zoneReports.erase(it) : ++it;
        }
    }

private:
    // 전용 writer 스레드: 큐에 쌓인 이벤트를 모아서 Redis에 기록한다.
    // RedisClient 내부 mutex가 saveEvent를 직렬화하므로 여기서는 락 불필요.
    void redisWriterLoop() {
        std::vector<PendingEvent> batch;
        while (m_redisWriterRunning) {
            {
                std::unique_lock<std::mutex> lock(m_redisQueueMutex);
                m_redisCv.wait_for(lock, std::chrono::seconds(1), [this] {
                    return !m_redisQueue.empty() || !m_redisWriterRunning;
                });
                while (!m_redisQueue.empty()) {
                    batch.push_back(m_redisQueue.front());
                    m_redisQueue.pop_front();
                }
            }
            for (const auto& ev : batch) {
                if (!m_redisWriterRunning) break;
                m_redis->saveEvent(ev.key, ev.userId, ev.unixTs);
            }
            batch.clear();
        }
    }
};

#endif // SPATIAL_DENSITY_ENGINE_H