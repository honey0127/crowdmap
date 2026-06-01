#ifndef SPATIAL_DENSITY_ENGINE_H
#define SPATIAL_DENSITY_ENGINE_H

#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>          // std::unique_lock (shared_mutex 헤더엔 없음)
#include <memory>
#include <chrono>
#include "SpatialHash.h"
#include "GridZone.h"

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

    // bleCount > 0 이면 BLE 감지 기기 수도 함께 기록 (기본값 0 = BLE 없음)
    void recordLocation(int32_t userId, double lat, double lon, int bleCount = 0) {
        int64_t key = SpatialHash::generateKey(lat, lon);
        auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];
        auto now = std::chrono::steady_clock::now();

        std::unique_lock<std::shared_mutex> lock(bucket->mutex);
        bucket->zones[key].update(userId, now);
        if (bleCount > 0) {
            bucket->zones[key].updateBle(bleCount, now);
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
    }
};

#endif // SPATIAL_DENSITY_ENGINE_H