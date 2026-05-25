#ifndef SPATIAL_DENSITY_ENGINE_H
#define SPATIAL_DENSITY_ENGINE_H

#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include "SpatialHash.h"
#include "GridZone.h"

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

public:
    SpatialDensityEngine() {
        m_buckets.reserve(NUM_STRIPES);
        for (size_t i = 0; i < NUM_STRIPES; ++i) {
            m_buckets.push_back(std::make_unique<Bucket>());
        }
    }

    /**
     * @brief 위치 정보 기록 및 밀도 업데이트 (Write-Intensive)
     */
    void recordLocation(int32_t userId, double lat, double lon) {
        int64_t key = SpatialHash::generateKey(lat, lon);
        // 키 해싱을 통한 버킷 인덱싱 (Lock-Striping)
        auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];
        auto now = std::chrono::steady_clock::now();
        
        // 해당 버킷만 독점 락 (다른 버킷은 병렬 접근 가능)
        std::unique_lock<std::shared_mutex> lock(bucket->mutex);
        bucket->zones[key].update(userId, now);
    }

    /**
     * @brief 특정 위치 및 주변 3x3 구역의 실시간 밀도 조회 (Read-Intensive)
     * 사용자 터치 오차를 보정하기 위해 인접 8개 그리드를 포함하여 합산합니다.
     */
    size_t getDensity(double lat, double lon) {
        size_t totalDensity = 0;

        // 클릭 지점 중심 3x3 그리드 스캔 (약 300m x 300m 범위)
        for (double dLat = -SpatialHash::GRID_SIZE; dLat <= SpatialHash::GRID_SIZE; dLat += SpatialHash::GRID_SIZE) {
            for (double dLon = -SpatialHash::GRID_SIZE; dLon <= SpatialHash::GRID_SIZE; dLon += SpatialHash::GRID_SIZE) {

                int64_t key = SpatialHash::generateKey(lat + dLat, lon + dLon);
                auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];

                // 개별 버킷에 대해 공유 락 획득 후 밀도 합산
                // (각 키마다 락을 획득/해제하므로 데드락 위험 없음)
                {
                    std::shared_lock<std::shared_mutex> lock(bucket->mutex);
                    auto it = bucket->zones.find(key);
                    if (it != bucket->zones.end()) {
                        totalDensity += it->second.getDensity();
                    }
                }
            }
        }
        return totalDensity;
    }

    /**
     * @brief 전역 데이터 정제 (Background cleanup 호출용)
     */
    void globalCleanup() {
        auto now = std::chrono::steady_clock::now();
        for (auto& bucket : m_buckets) {
            std::unique_lock<std::shared_mutex> lock(bucket->mutex);
            for (auto it = bucket->zones.begin(); it != bucket->zones.end(); ) {
                it->second.cleanup(now);
                if (it->second.getDensity() == 0) {
                    it = bucket->zones.erase(it); // 빈 구역 메모리 해제
                } else {
                    ++it;
                }
            }
        }
    }
};

#endif // SPATIAL_DENSITY_ENGINE_H
