package com.example.crowdmap.yeobaek.data

import com.google.gson.annotations.SerializedName

// ─────────────────────────────────────────────────────────────────────────────
// 여백 API DTO (부록 B 계약). Gson 직렬화, snake_case 는 @SerializedName 으로 매핑.
// ─────────────────────────────────────────────────────────────────────────────

// ── /api/v1/schedule ──
data class Weights(
    val congestion: Double = 1.0,
    val distance: Double = 0.3,
    val similarity: Double = 0.5,
)

data class ScheduleRequest(
    @SerializedName("start_time") val startTime: String,   // KST ISO "2026-10-11T10:00:00"
    val stops: List<Long>,
    val weights: Weights = Weights(),
    @SerializedName("allow_substitution") val allowSubstitution: Boolean = true,
)

data class PlanStop(
    @SerializedName("content_id") val contentId: Long,
    val title: String,
    val arrival: String,                                   // "HH:MM"
    @SerializedName("forecast_level") val forecastLevel: Int, // 1~4
    @SerializedName("substituted_from") val substitutedFrom: Long? = null,
)

data class ScheduleResponse(
    val ordered: List<PlanStop>,
    @SerializedName("total_cost") val totalCost: Double,
    @SerializedName("saved_congestion_pct") val savedCongestionPct: Int,
)

// ── /api/v1/match ──
data class MatchRequest(
    @SerializedName("content_id") val contentId: Long,
    @SerializedName("radius_km") val radiusKm: Double = 5.0,
    @SerializedName("top_k") val topK: Int = 3,
    @SerializedName("arrival_time") val arrivalTime: String? = null,
)

data class Source(
    @SerializedName("content_id") val contentId: Long,
    val title: String,
    val addr: String? = null,
    val lat: Double? = null,
    val lng: Double? = null,
    val cat: String? = null,
)

data class Twin(
    @SerializedName("content_id") val contentId: Long,
    val title: String,
    val similarity: Double,
    @SerializedName("forecast_level") val forecastLevel: Int,
    @SerializedName("dist_km") val distKm: Double,
)

data class MatchResponse(
    val source: Source,
    val twins: List<Twin>,
)

// ── /api/v1/card ──
data class CardRequest(
    @SerializedName("source_id") val sourceId: Long,
    @SerializedName("alt_id") val altId: Long,
    @SerializedName("arrival_time") val arrivalTime: String? = null,
)

data class GroundedFacts(
    @SerializedName("congestion_diff_pct") val congestionDiffPct: Int,
    @SerializedName("shared_category") val sharedCategory: String? = null,
)

data class CardResponse(
    val headline: String,
    val body: String,
    @SerializedName("grounded_facts") val groundedFacts: GroundedFacts,
    @SerializedName("generated_by") val generatedBy: String,   // "template" | "llm"
)

// 혼잡 레벨 유틸(뷰에서 배지 색·라벨에 사용)
object Congestion {
    fun label(level: Int): String = when (level) {
        1 -> "여유"; 2 -> "보통"; 3 -> "약간 붐빔"; 4 -> "붐빔"; else -> "보통"
    }
    /** 배지 배경 색상(ARGB). */
    fun color(level: Int): Int = when (level) {
        1 -> 0xFF2E7D32.toInt()  // green
        2 -> 0xFF1565C0.toInt()  // blue
        3 -> 0xFFEF6C00.toInt()  // orange
        4 -> 0xFFC62828.toInt()  // red
        else -> 0xFF616161.toInt()
    }
    fun isHigh(level: Int): Boolean = level >= 3
}
