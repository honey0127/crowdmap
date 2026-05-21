#ifndef ZONEMAPPER_H
#define ZONEMAPPER_H

#include <cmath>

class ZoneMapper {
private:
    // 서울 기준 (가로 약 30km, 세로 약 25km)
    static constexpr double GRID_SIZE = 0.01; // 약 1km씩 분할
    static constexpr double BASE_LAT = 37.5;
    static constexpr double BASE_LNG = 127.0;

public:
    // 위도, 경도를 zoneID로 변환. 서비스 범위 밖이면 -1 반환
    int coordinateToZoneId(double latitude, double longitude);
    // zoneID를 위도, 경도로 역변환 (선택사항)
    void zoneIdToCoordinate(int zoneId, double& latitude, double& longitude);
};

#endif // ZONEMAPPER_H