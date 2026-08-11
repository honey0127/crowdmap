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
    // keepOrder=true → 내가 고른 순서 그대로. false → 혼잡도 예측으로 자동 재배치.
    @SerializedName("keep_order") val keepOrder: Boolean = false,
)

data class PlanStop(
    @SerializedName("content_id") val contentId: Long,
    val title: String,
    val arrival: String,                                   // "HH:MM"
    @SerializedName("forecast_level") val forecastLevel: Int, // 1~4
    @SerializedName("substituted_from") val substitutedFrom: Long? = null,
    val lat: Double? = null,
    val lng: Double? = null,
)

data class ScheduleResponse(
    val ordered: List<PlanStop>,
    @SerializedName("total_cost") val totalCost: Double,
    @SerializedName("saved_congestion_pct") val savedCongestionPct: Int,
    // 여백 지수(0~100) — 혼잡·유사도·이동효율을 합산한 이 계획의 단일 점수(브랜드 지표).
    @SerializedName("yeobaek_index") val yeobaekIndex: Int? = null,
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

// ── /api/v1/places/search ──
data class PlaceResult(
    @SerializedName("content_id") val contentId: Long,
    val title: String,
    val addr: String? = null,
    @SerializedName("cat_label") val catLabel: String? = null,
    val lat: Double? = null,   // 지도 마커용
    val lng: Double? = null,
    @SerializedName("dist_km") val distKm: Double? = null,  // 지역 추천 거리
    val level: Int? = null,                                 // 현재 혼잡 레벨 1~4(히트맵)
    @SerializedName("quiet_score") val quietScore: Int? = null, // 한적함 지수 0~100
)

data class SearchResponse(val results: List<PlaceResult>)

// ── /api/v1/districts (지역구 라인업) ──
data class District(
    val key: String,
    val name: String,
    val lat: Double,
    val lng: Double,
    val desc: String? = null,
)

data class DistrictListResponse(val districts: List<District>)

data class DistrictPlacesResponse(
    val district: District,
    val places: List<PlaceResult>,
)

// ── /api/v1/places/disperse (미시적 분산) ──
data class DisperseResponse(
    @SerializedName("source_id") val sourceId: Long,
    val results: List<PlaceResult> = emptyList(),
)

// ── /api/v1/places/offpeak (오프피크 시간) ──
data class OffpeakHour(
    val unix: Long,
    val level: Int,
    @SerializedName("quiet_score") val quietScore: Int? = null,
    // "왜 이 시간?" 근거 — 서울시 원본 예보 값(있을 때만; SEOUL_API_KEY 미설정/예보 부재 시 null).
    @SerializedName("level_label") val levelLabel: String? = null,
    @SerializedName("ppltn_min") val ppltnMin: Int? = null,
    @SerializedName("ppltn_max") val ppltnMax: Int? = null,
    // true면 서울 API 예보창(~12h) 밖(D+1 등)이라 과거 같은 시간대 평균으로 추정한 값.
    val historical: Boolean = false,
)

data class OffpeakResponse(
    @SerializedName("content_id") val contentId: Long,
    val area: String? = null,
    val best: List<OffpeakHour> = emptyList(),
    val timeline: List<OffpeakHour> = emptyList(),
    val source: String? = null,   // 데이터 출처(원본 API 명) — 근거 화면에 표시
)

// ── /api/v1/resolve_now (실시간 혼잡, 리스케줄 알림) ──
data class ResolveNowResponse(
    val level: Int? = null,
    val valid: Boolean = false,
    @SerializedName("quiet_score") val quietScore: Int? = null,
)

// ── /api/v1/reports (여행자 실시간 제보) ──
data class ReportRequest(
    val kind: String,                 // busy | quiet | tip
    val lat: Double,
    val lng: Double,
    val text: String? = null,
    @SerializedName("content_id") val contentId: Long? = null,
)

data class ReportAck(val id: Long, val ok: Boolean = true)

data class ReportItem(
    val id: Long,
    val lat: Double,
    val lng: Double,
    val kind: String,
    val text: String? = null,
    @SerializedName("created_at") val createdAt: String? = null,
    @SerializedName("dist_km") val distKm: Double? = null,
)

data class ReportsResponse(val reports: List<ReportItem> = emptyList())

// ── /api/v1/places/adhoc (임의 위치 즉석 등록) ──
data class AdhocRequest(val title: String, val lat: Double, val lng: Double)

data class AdhocResponse(
    @SerializedName("content_id") val contentId: Long,
    val title: String,
    val lat: Double? = null,
    val lng: Double? = null,
    @SerializedName("area_name") val areaName: String? = null,
)

// 혼잡 레벨 유틸(뷰에서 배지 색·라벨에 사용)
object Congestion {
    fun label(level: Int): String = when (level) {
        1 -> "여유"; 2 -> "보통"; 3 -> "약간 붐빔"; 4 -> "붐빔"; else -> "보통"
    }
    /** 배지 배경 색상(ARGB) — 시맨틱 히트 스케일(여유→붐빔), 채도 낮춤(Compose 밀도 토큰과 통일). */
    fun color(level: Int): Int = when (level) {
        1 -> 0xFF3F8266.toInt()  // 여유 · 세이지
        2 -> 0xFFB4863F.toInt()  // 보통 · 앰버
        3 -> 0xFFAF585A.toInt()  // 약간 붐빔 · 뮤트 크림슨
        4 -> 0xFF93454A.toInt()  // 붐빔
        else -> 0xFF8A929C.toInt()
    }
    fun isHigh(level: Int): Boolean = level >= 3
    /** 구글맵 마커 색조(0~360). 여유=초록 … 붐빔=빨강. 레벨 없으면 브랜드 그린. */
    fun hue(level: Int?): Float = when (level) {
        1 -> 140f; 2 -> 48f; 3 -> 25f; 4 -> 8f; else -> 153f
    }
}
