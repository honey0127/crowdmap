#include "ZoneMapper.h"

int ZoneMapper::coordinateToZoneId(double latitude, double longitude) {
    // +500 오프셋으로 음수 인덱스를 방지 (기준점 남/서쪽도 안전하게 처리)
    int latIdx = static_cast<int>((latitude - BASE_LAT) / GRID_SIZE) + 500;
    int lngIdx = static_cast<int>((longitude - BASE_LNG) / GRID_SIZE) + 500;

    if (latIdx < 0 || latIdx >= 1000 || lngIdx < 0 || lngIdx >= 1000) {
        return -1; // 서비스 범위 밖 → 호출 측에서 처리
    }
    return latIdx * 1000 + lngIdx;
}

void ZoneMapper::zoneIdToCoordinate(int zoneId, double& latitude, double& longitude) {
    int latIdx = (zoneId / 1000) - 500;
    int lngIdx = (zoneId % 1000) - 500;

    latitude  = BASE_LAT + (latIdx * GRID_SIZE);
    longitude = BASE_LNG + (lngIdx * GRID_SIZE);
}