package com.example.crowdmap.model

data class CongestionData(
    val level: String,   // "RELAXED", "MODERATE", "CROWDED"
    val ratio: Double,   // 0.0 ~ 1.0
    val zoneId: Int
) {
    // 혼잡도 레벨을 한국어로 변환
    fun levelKorean(): String {
        return when (level) {
            "RELAXED"  -> "여유"
            "MODERATE" -> "보통"
            "CROWDED"  -> "혼잡"
            else       -> "알 수 없음"
        }
    }

    // 혼잡도에 따른 색상 (지도 표시용)
    fun color(): Int {
        return when (level) {
            "RELAXED"  -> 0xFF00AA00.toInt() // 초록
            "MODERATE" -> 0xFFFFAA00.toInt() // 노랑
            "CROWDED"  -> 0xFFFF0000.toInt() // 빨강
            else       -> 0xFF888888.toInt() // 회색
        }
    }
}