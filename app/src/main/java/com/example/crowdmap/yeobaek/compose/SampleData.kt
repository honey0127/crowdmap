package com.example.crowdmap.yeobaek.compose

import com.example.crowdmap.yeobaek.data.CardResponse
import com.example.crowdmap.yeobaek.data.GroundedFacts
import com.example.crowdmap.yeobaek.data.MatchResponse
import com.example.crowdmap.yeobaek.data.OffpeakHour
import com.example.crowdmap.yeobaek.data.OffpeakResponse
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.PlanStop
import com.example.crowdmap.yeobaek.data.ScheduleResponse
import com.example.crowdmap.yeobaek.data.Source
import com.example.crowdmap.yeobaek.data.Twin

/**
 * @Preview 및 오프라인 시연용 샘플 데이터.
 * 실측 서울 명소(경복궁·북촌·서촌…)와 파이프라인 검증에서 확인된 값 범위를 사용한다.
 * 실제 화면은 이 형태 그대로 서버 응답(YeobaekApi)을 받아 렌더한다.
 */
object SampleData {

    // /api/v1/match — 북촌(붐빔) → 서촌(한적) 감성 쌍둥이
    val match = MatchResponse(
        source = Source(
            contentId = 900005, title = "북촌한옥마을", addr = "서울 종로구 계동길",
            lat = 37.582604, lng = 126.985302, cat = "한옥 · 전통마을",
        ),
        twins = listOf(
            Twin(900105, "서촌 한옥골목", similarity = 0.91, forecastLevel = 1, distKm = 1.2),
            Twin(900006, "익선동 한옥거리", similarity = 0.87, forecastLevel = 2, distKm = 1.8),
            Twin(900007, "남산골한옥마을", similarity = 0.82, forecastLevel = 1, distKm = 2.6),
        ),
    )

    // 히어로 현재 혼잡(리스케줄 기준) — 북촌 78% / Lv.4
    const val heroPct = 78
    const val heroLevel = 4

    // /api/v1/card — 설득 카드
    val card = CardResponse(
        headline = "붐비는 북촌 대신, 고요한 서촌 한옥골목은 어때요?",
        body = "같은 한옥·전통마을 감성이지만 지금 이 시각 혼잡은 56%p 더 낮습니다. 골목의 결과 정취는 그대로, 사람만 덜합니다.",
        groundedFacts = GroundedFacts(congestionDiffPct = 56, sharedCategory = "한옥 · 전통마을"),
        generatedBy = "template",
    )

    // /api/v1/schedule — 최적화된 하루
    val schedule = ScheduleResponse(
        ordered = listOf(
            PlanStop(900001, "경복궁", arrival = "10:00", forecastLevel = 1, lat = 37.5796, lng = 126.9770),
            PlanStop(900105, "서촌 한옥골목", arrival = "11:45", forecastLevel = 1, substitutedFrom = 900005, lat = 37.5789, lng = 126.9702),
            PlanStop(900016, "국립중앙박물관", arrival = "13:00", forecastLevel = 2, lat = 37.5240, lng = 126.9804),
            PlanStop(900014, "서울숲", arrival = "15:30", forecastLevel = 1, lat = 37.5446, lng = 127.0417),
        ),
        totalCost = 1.0079,
        savedCongestionPct = 43,
    )

    // 스톱별 부가 표시값(체류·이동) — 서버 확장 전까지 클라이언트 근사(결정 D dwell).
    val dwellMin = listOf(90, 60, 75, 60)
    val transit = listOf("도보 8분 · 240m", "버스 12분 · 2.1km", "지하철 16분 · 5.4km")

    // /api/v1/places/heatmap — 지도 히트맵(현재 혼잡 레벨 + 한적함 지수)
    val heatmap = listOf(
        PlaceResult(900005, "북촌한옥마을", catLabel = "한옥마을", lat = 37.5826, lng = 126.9853, level = 4, quietScore = 18),
        PlaceResult(900105, "서촌 한옥골목", catLabel = "한옥마을", lat = 37.5789, lng = 126.9702, level = 1, quietScore = 82),
        PlaceResult(900001, "경복궁", catLabel = "고궁", lat = 37.5796, lng = 126.9770, level = 1, quietScore = 74),
        PlaceResult(900004, "창경궁", catLabel = "고궁", lat = 37.5786, lng = 126.9948, level = 2, quietScore = 61),
        PlaceResult(900008, "명동거리", catLabel = "번화가", lat = 37.5636, lng = 126.9850, level = 4, quietScore = 12),
        PlaceResult(900006, "익선동 한옥거리", catLabel = "한옥마을", lat = 37.5744, lng = 126.9910, level = 2, quietScore = 58),
        PlaceResult(900018, "인사동길", catLabel = "문화거리", lat = 37.5738, lng = 126.9856, level = 3, quietScore = 34),
    )

    // /api/v1/places/offpeak — 서촌 시간대별 혼잡(오프피크 추천)
    val offpeak = OffpeakResponse(
        contentId = 900105, area = "서촌·경복궁",
        best = listOf(
            OffpeakHour(unix = 0, level = 1, quietScore = 88),
            OffpeakHour(unix = 0, level = 1, quietScore = 81),
        ),
        timeline = listOf(
            OffpeakHour(unix = 0, level = 1, quietScore = 88), // 09
            OffpeakHour(unix = 0, level = 1, quietScore = 79), // 10
            OffpeakHour(unix = 0, level = 2, quietScore = 61), // 11
            OffpeakHour(unix = 0, level = 3, quietScore = 38), // 12
            OffpeakHour(unix = 0, level = 3, quietScore = 33), // 13
            OffpeakHour(unix = 0, level = 2, quietScore = 55), // 14
            OffpeakHour(unix = 0, level = 1, quietScore = 72), // 15
            OffpeakHour(unix = 0, level = 1, quietScore = 84), // 16
        ),
    )
    val offpeakHours = listOf("09", "10", "11", "12", "13", "14", "15", "16")

    // 상세 화면 대상 장소
    val detailPlace = PlaceResult(
        900105, "서촌 한옥골목", addr = "서울 종로구 자하문로", catLabel = "한옥 · 전통마을",
        lat = 37.5789, lng = 126.9702, level = 1, quietScore = 82,
    )
}
