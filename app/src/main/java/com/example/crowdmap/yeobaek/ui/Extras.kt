package com.example.crowdmap.yeobaek.ui

// 액티비티 간 전달 키. 계획(Plan)은 JSON 문자열로 넘겨 Parcelable 보일러플레이트를 피한다.
object Extras {
    const val STOPS = "stops"            // LongArray — 현재 방문지(스왑 시 재스케줄용)
    const val START_TIME = "start_time"  // String   — KST ISO
    const val PLAN_JSON = "plan_json"    // String   — ScheduleResponse(JSON)
    const val KEEP_ORDER = "keep_order"  // Boolean  — true=내 순서대로, false=자동 최적화
    const val TARGET_ID = "target_id"    // Long     — 대안을 찾을 stop
    const val SOURCE_ID = "source_id"    // Long
    const val ALT_ID = "alt_id"          // Long
    const val PLACE_ID = "place_id"      // Long   — SearchActivity 결과
    const val PLACE_TITLE = "place_title" // String — SearchActivity 결과
    const val PLACE_LAT = "place_lat"    // Double — SearchActivity 결과(지도 마커)
    const val PLACE_LNG = "place_lng"    // Double — SearchActivity 결과(지도 마커)
    // 위치 기준 코스 플래너(DistrictActivity)에 넘길 시작 좌표·이름
    const val CENTER_LAT = "center_lat"  // Double
    const val CENTER_LNG = "center_lng"  // Double
    const val CENTER_NAME = "center_name" // String
}
