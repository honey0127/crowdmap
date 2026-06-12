package com.example.crowdmap.signal // TODO: 실제 패키지명으로 변경

import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorManager
import android.hardware.TriggerEvent
import android.hardware.TriggerEventListener
import android.os.Build
import android.util.Log
import com.google.android.gms.location.ActivityRecognition
import com.google.android.gms.location.ActivityTransition
import com.google.android.gms.location.ActivityTransitionRequest
import com.google.android.gms.location.ActivityTransitionResult
import com.google.android.gms.location.DetectedActivity

/**
 * [우선순위 2] 정지/이동 게이팅 — 전력 절감 효과 최대 항목.
 *
 * 동작:
 *  - Activity Recognition이 STILL 진입 감지 → onStill() 콜백
 *    (호출측: GPS 요청 중단, heartbeat 송신만 유지 — 서버 5분 윈도우 증발 방지는 기존 heartbeat가 담당)
 *  - STILL 이탈 또는 Significant Motion 센서(하드웨어 인터럽트, 전력 ≈ 0) 발화 → onMoving()
 *    (호출측: GPS 재개, EMA/히스테리시스 필터 reset)
 *
 * 축제 군중은 대부분의 시간 정지 상태이므로 평균 GPS 듀티가 크게 감소.
 *
 * 필요 권한: ACTIVITY_RECOGNITION (API 29+, 런타임)
 * 필요 의존성: play-services-location (이미 사용 중)
 */
class MotionGate(
    private val context: Context,
    private val onStill: () -> Unit,
    private val onMoving: () -> Unit
) {
    private val sensorManager =
        context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private val sigMotion: Sensor? =
        sensorManager.getDefaultSensor(Sensor.TYPE_SIGNIFICANT_MOTION)

    private var started = false
    private var pendingIntent: PendingIntent? = null

    /** Significant Motion: one-shot 트리거 — 발화 즉시 이동으로 판정 (AR보다 반응 빠름) */
    private val sigMotionListener = object : TriggerEventListener() {
        override fun onTrigger(event: TriggerEvent?) {
            Log.d(TAG, "significant motion → moving")
            onMoving()
            // one-shot이므로 재장전은 다음 STILL 진입 시
        }
    }

    private val transitionReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != ACTION_TRANSITION) return
            if (!ActivityTransitionResult.hasResult(intent)) return
            val result = ActivityTransitionResult.extractResult(intent) ?: return
            for (e in result.transitionEvents) {
                if (e.activityType != DetectedActivity.STILL) continue
                if (e.transitionType == ActivityTransition.ACTIVITY_TRANSITION_ENTER) {
                    Log.d(TAG, "STILL enter → still")
                    onStill()
                    armSignificantMotion()   // 정지 확정 → 하드웨어 깨우기 트리거 장전
                } else {
                    Log.d(TAG, "STILL exit → moving")
                    onMoving()
                }
            }
        }
    }

    @SuppressLint("MissingPermission", "UnspecifiedRegisterReceiverFlag")
    fun start() {
        if (started) return
        started = true

        val filter = IntentFilter(ACTION_TRANSITION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            context.registerReceiver(transitionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            context.registerReceiver(transitionReceiver, filter)
        }

        val intent = Intent(ACTION_TRANSITION).setPackage(context.packageName)
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                (if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) PendingIntent.FLAG_MUTABLE else 0)
        pendingIntent = PendingIntent.getBroadcast(context, RC, intent, flags)

        val transitions = listOf(
            ActivityTransition.Builder()
                .setActivityType(DetectedActivity.STILL)
                .setActivityTransition(ActivityTransition.ACTIVITY_TRANSITION_ENTER)
                .build(),
            ActivityTransition.Builder()
                .setActivityType(DetectedActivity.STILL)
                .setActivityTransition(ActivityTransition.ACTIVITY_TRANSITION_EXIT)
                .build()
        )
        ActivityRecognition.getClient(context)
            .requestActivityTransitionUpdates(ActivityTransitionRequest(transitions), pendingIntent!!)
            .addOnFailureListener { Log.w(TAG, "AR registration failed (permission?): $it") }
    }

    @SuppressLint("MissingPermission")
    fun stop() {
        if (!started) return
        started = false
        pendingIntent?.let {
            ActivityRecognition.getClient(context).removeActivityTransitionUpdates(it)
            it.cancel()
        }
        pendingIntent = null
        runCatching { context.unregisterReceiver(transitionReceiver) }
        sigMotion?.let { sensorManager.cancelTriggerSensor(sigMotionListener, it) }
    }

    private fun armSignificantMotion() {
        sigMotion?.let { sensorManager.requestTriggerSensor(sigMotionListener, it) }
    }

    private companion object {
        const val TAG = "MotionGate"
        const val ACTION_TRANSITION = "com.example.crowdmap.ACTIVITY_TRANSITION" // TODO: 패키지명 정합
        const val RC = 7001
    }
}