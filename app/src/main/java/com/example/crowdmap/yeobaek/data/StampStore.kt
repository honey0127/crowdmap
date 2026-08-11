package com.example.crowdmap.yeobaek.data

import android.content.Context

/**
 * 여백 스탬프(로컬) — 담은 장소마다 스탬프 1개. 서버 없이 로컬 저장만으로 동작하는
 * 리텐션 장치(포인트/배지 [EcoStore] 와 별개로, "이 장소를 모았다"는 수집 자체가 보상).
 */
object StampStore {
    private const val PREF = "yeobaek_stamps"
    private const val K_STAMPS = "stamps"   // Set<"contentId|title">

    private fun prefs(ctx: Context) =
        ctx.getSharedPreferences(PREF, Context.MODE_PRIVATE)

    /** 이미 찍은 스탬프면 false(중복 없음), 새로 찍었으면 true. */
    fun stamp(ctx: Context, contentId: Long, title: String): Boolean {
        val p = prefs(ctx)
        val cur = p.getStringSet(K_STAMPS, emptySet())!!
        if (cur.any { it.substringBefore('|') == contentId.toString() }) return false
        val next = cur.toMutableSet().apply { add("$contentId|$title") }
        p.edit().putStringSet(K_STAMPS, next).apply()
        return true
    }

    data class Stamp(val contentId: Long, val title: String)

    fun all(ctx: Context): List<Stamp> =
        prefs(ctx).getStringSet(K_STAMPS, emptySet())!!.mapNotNull { entry ->
            val id = entry.substringBefore('|').toLongOrNull() ?: return@mapNotNull null
            Stamp(id, entry.substringAfter('|', missingDelimiterValue = "이름 없음"))
        }.sortedBy { it.title }

    fun count(ctx: Context): Int = prefs(ctx).getStringSet(K_STAMPS, emptySet())!!.size
}
