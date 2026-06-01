#ifndef SPATIAL_DENSITY_ENGINE_H
#define SPATIAL_DENSITY_ENGINE_H

#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <memory>
#include <chrono>
#include "SpatialHash.h"
#include "GridZone.h"

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
};

#endif // SPATIAL_DENSITY_ENGINE_H