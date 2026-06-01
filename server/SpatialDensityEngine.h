#ifndef SPATIAL_DENSITY_ENGINE_H
#define SPATIAL_DENSITY_ENGINE_H

#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "GridZone.h"
#include "Logger.h"
#include "RedisClient.h"
#include "SpatialHash.h"

/**
 * @brief Spatial-Concurrency Data Layer 엔진
 * Lock-Striping(1024개 버킷)을 통해 락 경합을 최소화하며
 * 실시간 공간 밀도를 집계합니다.
 */
class SpatialDensityEngine {
private:
    struct Bucket {
        std::shared_mutex mutex;
        std::unordered_map<int64_t, GridZone> zones;
    };

    static constexpr size_t NUM_STRIPES = 1024;
    std::vector<std::unique_ptr<Bucket>> m_buckets;
    RedisClient* m_redis = nullptr; // nullptr이면 영속성 비활성화

public:
    SpatialDensityEngine() {
        m_buckets.reserve(NUM_STRIPES);
        for (size_t i = 0; i < NUM_STRIPES; ++i) {
            m_buckets.push_back(std::make_unique<Bucket>());
        }
    }

    // main()에서 Redis 클라이언트 주입
    void setRedis(RedisClient* redis) { m_redis = redis; }

    /**
     * @brief 위치 정보 기록 및 밀도 업데이트 (Write-Intensive)
     * Redis가 설정된 경우 동시에 영속화합니다.
     */
    void recordLocation(int32_t userId, double lat, double lon) {
        int64_t key = SpatialHash::generateKey(lat, lon);
        auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];
        auto now = std::chrono::steady_clock::now();

        {
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            bucket->zones[key].update(userId, now);
        }

        if (m_redis) {
            long long unixTs = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            m_redis->saveEvent(key, userId, unixTs);
        }
    }

    /**
     * @brief 서버 시작 시 Redis에서 이전 이벤트를 복원합니다.
     * steady_clock은 프로세스 상대 시간이므로, unix 타임스탬프의 경과 시간을
     * 계산해 동등한 steady_clock 시점으로 변환합니다.
     */
    void restoreFromRedis(RedisClient& redis) {
        auto events = redis.loadAllEvents();
        if (events.empty()) {
            Log::info("[Redis] 복원할 이벤트 없음");
            return;
        }

        long long nowUnix = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto nowSteady = std::chrono::steady_clock::now();

        int restored = 0;
        for (auto& [spatialKey, userId, storedUnix] : events) {
            long long ageSeconds = nowUnix - storedUnix;
            if (ageSeconds < 0 || ageSeconds >= 300) continue;

            auto originalSteady = nowSteady - std::chrono::seconds(ageSeconds);

            auto& bucket = m_buckets[static_cast<uint64_t>(spatialKey) % NUM_STRIPES];
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            bucket->zones[spatialKey].injectHistorical(userId, originalSteady);
            ++restored;
        }

        Log::info("[Redis] 이벤트 " + std::to_string(restored) + "건 복원 완료");
        globalCleanup();
    }

    // [B-5] getDensity() 락 획득 방식 최적화:
    // 3×3 셀 키를 버킷 인덱스별로 먼저 그룹핑 → 버킷당 1번만 shared_lock 획득
    size_t getDensity(double lat, double lon) {
        size_t totalDensity = 0;

        // 1단계: 3×3 인근 셀 키를 버킷 인덱스별로 그룹핑
        std::unordered_map<size_t, std::vector<int64_t>> bucketKeys;
        for (double dLat = -SpatialHash::GRID_SIZE;
             dLat <= SpatialHash::GRID_SIZE + 1e-12;
             dLat += SpatialHash::GRID_SIZE) {
            for (double dLon = -SpatialHash::GRID_SIZE;
                 dLon <= SpatialHash::GRID_SIZE + 1e-12;
                 dLon += SpatialHash::GRID_SIZE) {
                int64_t key = SpatialHash::generateKey(lat + dLat, lon + dLon);
                size_t  idx = static_cast<uint64_t>(key) % NUM_STRIPES;
                bucketKeys[idx].push_back(key);
            }
        }

        // 2단계: 버킷별로 shared_lock 1회 획득 후 해당 버킷의 키 모두 조회
        for (auto& [idx, keys] : bucketKeys) {
            std::shared_lock<std::shared_mutex> lock(m_buckets[idx]->mutex);
            for (int64_t key : keys) {
                auto it = m_buckets[idx]->zones.find(key);
                if (it != m_buckets[idx]->zones.end()) {
                    totalDensity += it->second.getDensity();
                }
            }
        }

        return totalDensity;
    }

    void globalCleanup() {
        auto now = std::chrono::steady_clock::now();
        for (auto& bucket : m_buckets) {
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            for (auto it = bucket->zones.begin(); it != bucket->zones.end(); ) {
                it->second.cleanup(now);
                if (it->second.getDensity() == 0) {
                    it = bucket->zones.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
};

#endif // SPATIAL_DENSITY_ENGINE_H
