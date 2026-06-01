package com.example.crowdmap.location

/**
 * 서버 ZoneMapper와 동일한 공식으로 클라이언트에서 zoneId를 계산합니다.
 *
 * 서버: ZoneMapper::coordinateToZoneId()
 *   latIdx = (lat - 37.5) / 0.01 + 500
 *   lngIdx = (lng - 127.0) / 0.01 + 500
 *   zoneId = latIdx * 1000 + lngIdx
 */
object ZoneIdCalculator {

    private const val BASE_LAT   = 37.5
    private const val BASE_LNG   = 127.0
    private const val GRID_SIZE  = 0.01  // 약 1km

    /** 좌표 → zoneId. 서비스 범위 밖이면 -1 반환 */
    fun fromCoordinate(lat: Double, lng: Double): Int {
        val latIdx = ((lat - BASE_LAT) / GRID_SIZE).toInt() + 500
        val lngIdx = ((lng - BASE_LNG) / GRID_SIZE).toInt() + 500
        if (latIdx < 0 || latIdx >= 1000 || lngIdx < 0 || lngIdx >= 1000) return -1
        return latIdx * 1000 + lngIdx
    }
}
