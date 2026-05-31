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

    // [B-5 수정] getDensity() 락 획득 방식 최적화
    //
    // 문제: 기존 코드는 3×3 루프에서 셀마다 개별 락/언락 반복
    //       → 9개 셀이 우연히 같은 버킷에 속하면 같은 버킷을 최대 9번 락
    //       → 불필요한 락 경합 및 오버헤드 발생
    //
    // 해결: 1단계에서 9개 셀 키를 버킷 인덱스별로 먼저 그룹핑
    //       2단계에서 버킷당 1번만 락 획득 후 해당 버킷의 키 모두 조회
    //       → 3×3 합산 기능은 동일하게 유지, 락 획득 횟수만 최소화
    size_t getDensity(double lat, double lon) {
        size_t totalDensity = 0;

        // 1단계: 3×3 인근 셀(주변 9칸)의 키를 버킷 인덱스별로 그룹핑
        // → 같은 버킷에 속하는 키끼리 묶어서 락을 버킷당 1번만 획득하기 위함
        std::unordered_map<size_t, std::vector<int64_t>> bucketKeys;
        for (double dLat = -SpatialHash::GRID_SIZE;
             dLat <= SpatialHash::GRID_SIZE + 1e-12;  // 부동소수점 오차 보정
             dLat += SpatialHash::GRID_SIZE) {
            for (double dLon = -SpatialHash::GRID_SIZE;
                 dLon <= SpatialHash::GRID_SIZE + 1e-12;
                 dLon += SpatialHash::GRID_SIZE) {
                int64_t key = SpatialHash::generateKey(lat + dLat, lon + dLon);
                size_t  idx = static_cast<uint64_t>(key) % NUM_STRIPES;
                bucketKeys[idx].push_back(key);  // 버킷 인덱스 기준으로 키 분류
            }
        }

        // 2단계: 버킷별로 shared_lock 1회 획득 후 해당 버킷의 키 모두 조회
        // → shared_lock: 읽기 전용이므로 여러 스레드 동시 읽기 허용
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

#endif 