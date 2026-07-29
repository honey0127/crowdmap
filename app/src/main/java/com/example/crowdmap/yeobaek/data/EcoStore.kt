package com.example.crowdmap.yeobaek.data

import android.content.Context

/**
 * 에코 트래블러 리워드(로컬) — 혼잡을 피하는 선택에 포인트/배지를 준다.
 * (실제 상점 쿠폰 연동은 제휴가 필요 — 포인트/배지는 로컬로 먼저 제공)
 */
object EcoStore {
    private const val PREF = "yeobaek_eco"
    private const val K_POINTS = "points"

    // 행동별 포인트
    const val P_QUIET_ADD = 10      // 한적한 곳(여유/보통) 담기
    const val P_OFFPEAK = 15        // 오프피크 시간 선택
    const val P_DISPERSE = 20       // 도보권 한적한 대안 선택
    const val P_REPORT = 5          // 실시간 제보

    private fun prefs(ctx: Context) =
        ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE)

    fun points(ctx: Context): Int = prefs(ctx).getInt(K_POINTS, 0)

    /** 포인트 적립 후 누적 포인트 반환. */
    fun award(ctx: Context, pts: Int): Int {
        val next = points(ctx) + pts
        prefs(ctx).edit().putInt(K_POINTS, next).apply()
        return next
    }

    fun badge(points: Int): String = when {
        points >= 500 -> "에코 마스터"
        points >= 200 -> "에코 트래블러"
        points >= 50 -> "새싹 여행자"
        else -> "입문 여행자"
    }
}
