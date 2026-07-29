package com.example.crowdmap.yeobaek.data

import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
import retrofit2.http.Path
import retrofit2.http.Query

// 여백 FastAPI 계약(부록 B). suspend 함수 — Retrofit 2.6+ 코루틴 네이티브 지원.
interface YeobaekApi {
    @POST("api/v1/schedule")
    suspend fun schedule(@Body req: ScheduleRequest): ScheduleResponse

    @POST("api/v1/match")
    suspend fun match(@Body req: MatchRequest): MatchResponse

    @POST("api/v1/card")
    suspend fun card(@Body req: CardRequest): CardResponse

    @GET("api/v1/places/search")
    suspend fun searchPlaces(
        @Query("q") q: String,
        @Query("limit") limit: Int = 20,
    ): SearchResponse

    // 지도 이동 시 '이 지역 추천'
    @GET("api/v1/places/nearby")
    suspend fun nearbyPlaces(
        @Query("lat") lat: Double,
        @Query("lng") lng: Double,
        @Query("radius_km") radiusKm: Double = 3.0,
        @Query("limit") limit: Int = 12,
    ): SearchResponse

    // 주변 명소 + 현재 혼잡 레벨·한적함 지수(히트맵/핀 색)
    @GET("api/v1/places/heatmap")
    suspend fun heatmap(
        @Query("lat") lat: Double,
        @Query("lng") lng: Double,
        @Query("radius_km") radiusKm: Double = 3.0,
        @Query("limit") limit: Int = 24,
    ): SearchResponse

    // 지역구 목록/라인업 (플래너 페이지)
    @GET("api/v1/districts")
    suspend fun districts(): DistrictListResponse

    @GET("api/v1/districts/{key}")
    suspend fun districtPlaces(@Path("key") key: String): DistrictPlacesResponse

    // DB 에 없는 임의 위치 즉석 등록 → content_id
    @POST("api/v1/places/adhoc")
    suspend fun addAdhoc(@Body req: AdhocRequest): AdhocResponse
}
