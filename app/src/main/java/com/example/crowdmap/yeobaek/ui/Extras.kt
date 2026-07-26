package com.example.crowdmap.yeobaek.ui

// 액티비티 간 전달 키. 계획(Plan)은 JSON 문자열로 넘겨 Parcelable 보일러플레이트를 피한다.
object Extras {
    const val STOPS = "stops"            // LongArray — 현재 방문지(스왑 시 재스케줄용)
    const val START_TIME = "start_time"  // String   — KST ISO
    const val PLAN_JSON = "plan_json"    // String   — ScheduleResponse(JSON)
    const val TARGET_ID = "target_id"    // Long     — 대안을 찾을 stop
    const val SOURCE_ID = "source_id"    // Long
    const val ALT_ID = "alt_id"          // Long
}
