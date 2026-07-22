#ifndef YEOBAEK_SPATIAL_HASH_H
#define YEOBAEK_SPATIAL_HASH_H

#include <cstdint>
#include <cmath>

// [이식] CrowdMap SpatialHash — 위경도를 0.001도(약 100m) 그리드 64비트 키로 변환.
class SpatialHash {
public:
    static constexpr double GRID_SIZE = 0.001; // 약 111m (위도 기준)

    static int64_t generateKey(double lat, double lon) {
        int64_t latIdx = static_cast<int64_t>(std::floor(lat / GRID_SIZE));
        int64_t lonIdx = static_cast<int64_t>(std::floor(lon / GRID_SIZE));
        return (latIdx << 32) | (static_cast<uint32_t>(lonIdx));
    }

    static int64_t cellKey(int64_t latIdx, int64_t lonIdx) {
        return (latIdx << 32) | (static_cast<uint32_t>(lonIdx));
    }
    static int64_t latIndex(double lat) { return static_cast<int64_t>(std::floor(lat / GRID_SIZE)); }
    static int64_t lonIndex(double lon) { return static_cast<int64_t>(std::floor(lon / GRID_SIZE)); }
};

#endif // YEOBAEK_SPATIAL_HASH_H
