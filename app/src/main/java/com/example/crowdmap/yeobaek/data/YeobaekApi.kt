package com.example.crowdmap.yeobaek.data

import retrofit2.http.Body
import retrofit2.http.GET
import retrofit2.http.POST
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
}
