#include "ZoneMapper.h"

int ZoneMapper::coordinateToZoneId(double latitude, double longitude) {
    // 기준점으로부터의 거리를 격자 단위로 계산
    int latIdx = static_cast<int>((latitude - BASE_LAT) / GRID_SIZE);
    int lngIdx = static_cast<int>((longitude - BASE_LNG) / GRID_SIZE);
    
    // zoneID = lat_idx * 1000 + lng_idx (2D 좌표를 1D로 변환)
    int zoneId = latIdx * 1000 + lngIdx;
    return zoneId;
}

void ZoneMapper::zoneIdToCoordinate(int zoneId, double& latitude, double& longitude) {
    int latIdx = zoneId / 1000;
    int lngIdx = zoneId % 1000;
    
    latitude = BASE_LAT + (latIdx * GRID_SIZE);
    longitude = BASE_LNG + (lngIdx * GRID_SIZE);
}