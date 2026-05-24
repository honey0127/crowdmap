#ifndef SPATIAL_HASH_H
#define SPATIAL_HASH_H

#include <cstdint>
#include <cmath>

/**
 * @brief O(1) 공간 해싱 클래스
 * 위경도 좌표를 0.001도(약 100m) 그리드 단위의 64비트 정수 키로 변환합니다.
 */
class SpatialHash {
public:
    static constexpr double GRID_SIZE = 0.001; // 약 111m (위도 기준)

    static int64_t generateKey(double lat, double lon) {
        // 음수 좌표 처리를 위해 floor 사용
        int64_t latIdx = static_cast<int64_t>(std::floor(lat / GRID_SIZE));
        int64_t lonIdx = static_cast<int64_t>(std::floor(lon / GRID_SIZE));
        
        // 상위 32비트에 위도 인덱스, 하위 32비트에 경도 인덱스 배치
        return (latIdx << 32) | (static_cast<uint32_t>(lonIdx));
    }
};

#endif // SPATIAL_HASH_H
