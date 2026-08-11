package com.example.crowdmap.yeobaek.data

import android.content.Context
import com.google.gson.Gson

/**
 * 마지막으로 성공한 일정(ScheduleResponse)을 로컬에 저장 — 서버가 죽어도(심사·시연 중 포함)
 * 앱이 빈 화면/에러만 보여주지 않도록 하는 오프라인 폴백. Room 없이 EcoStore/StampStore 와
 * 같은 SharedPreferences+Gson 패턴을 따른다(신규 의존성 없음).
 */
object PlanCache {
    private const val PREF = "yeobaek_plan_cache"
    private const val K_SCHEDULE = "last_schedule"
    private val gson = Gson()

    fun save(ctx: Context, schedule: ScheduleResponse) {
        ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE).edit()
            .putString(K_SCHEDULE, gson.toJson(schedule))
            .apply()
    }

    fun load(ctx: Context): ScheduleResponse? {
        val json = ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE).getString(K_SCHEDULE, null)
            ?: return null
        return runCatching { gson.fromJson(json, ScheduleResponse::class.java) }.getOrNull()
    }
}
