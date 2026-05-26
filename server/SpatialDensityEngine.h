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

    void recordLocation(int32_t userId, double lat, double lon) {
        int64_t key = SpatialHash::generateKey(lat, lon);
        auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];
        auto now = std::chrono::steady_clock::now();

        std::unique_lock<std::shared_mutex> lock(bucket->mutex);
        bucket->zones[key].update(userId, now);
    }

    size_t getDensity(double lat, double lon) {
        size_t totalDensity = 0;

        for (double dLat = -SpatialHash::GRID_SIZE; dLat <= SpatialHash::GRID_SIZE; dLat += SpatialHash::GRID_SIZE) {
            for (double dLon = -SpatialHash::GRID_SIZE; dLon <= SpatialHash::GRID_SIZE; dLon += SpatialHash::GRID_SIZE) {

                int64_t key = SpatialHash::generateKey(lat + dLat, lon + dLon);
                auto& bucket = m_buckets[static_cast<uint64_t>(key) % NUM_STRIPES];

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