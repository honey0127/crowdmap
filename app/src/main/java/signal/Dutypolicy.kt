package com.example.crowdmap.signal // TODO: 실제 패키지명으로 변경

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager

/**
 * [우선순위 4] 단말 상태 적응형 듀티 정책.
 *  - 충전 중: 적극 수집 (데이터 품질↑, 비용 무의미)
 *  - 배터리 < 20%: 보존 모드 (수집을 줄이되 끊지는 않음 — 표본 유지)
 *  - 평시: 기존 듀티 유지
 *
 * 서버 주도 감압(interval= 힌트, honey에 기 구현)과의 결합 규칙:
 *  최종 배치 주기 = max(서버 힌트, 배터리 정책 하한)
 *  → 서버가 폭증 감압 중이면 서버가 이기고, 단말이 빈사 상태면 단말이 이김.
 */
data class Duty(
    val bleScanOnMs: Long,      // BLE 듀티 사이클 ON 구간
    val blePeriodMs: Long,      // BLE 듀티 사이클 주기
    val batchPeriodFloorMs: Long, // 배치 송신 주기 하한
    val locationIntervalMs: Long  // FusedLocation 요청 간격
)

object DutyPolicy {

    private val AGGRESSIVE = Duty(15_000, 30_000, 10_000, 5_000)  // 충전 중
    private val NORMAL     = Duty( 8_000, 30_000, 10_000, 10_000) // 현행과 동일
    private val CONSERVE   = Duty( 5_000, 60_000, 30_000, 20_000) // 저전력

    /** sticky broadcast 조회 — 리시버 등록 없이 현재 배터리 상태 1회 읽기 (호출 비용 미미) */
    fun current(context: Context): Duty {
        val intent = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val status = intent?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL
        val level = intent?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = intent?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val pct = if (level >= 0 && scale > 0) level * 100 / scale else 100
        return when {
            charging -> AGGRESSIVE
            pct < 20 -> CONSERVE
            else     -> NORMAL
        }
    }

    /** 서버 힌트와 결합한 최종 배치 주기 */
    fun effectiveBatchPeriodMs(context: Context, serverHintMs: Long): Long =
        maxOf(serverHintMs, current(context).batchPeriodFloorMs)
}