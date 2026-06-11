package com.example.crowdmap.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import androidx.core.content.ContextCompat
import com.example.crowdmap.MainActivity
import com.example.crowdmap.R
import com.example.crowdmap.core.TrackingCore

/**
 * [TrackingService]
 * 위치 추적을 포그라운드 서비스로 승격해 화면이 꺼지거나 앱이
 * 백그라운드로 가도 수집·전송 파이프라인(TrackingCore)이 계속 돌게 한다.
 *
 * 군중 밀집도 앱의 핵심 데이터원은 "주머니 속 폰"이다. Activity 수명에
 * 묶여 있으면 사용자가 화면을 끄는 순간 데이터가 끊겨, 정작 밀집
 * 상황(축제/재난)에서 표본이 사라진다.
 *
 * foregroundServiceType="location" 선언으로 while-in-use 위치 권한만으로도
 * 백그라운드 위치 수집이 허용된다 (Android 10+ 정책).
 */
class TrackingService : Service() {

    companion object {
        private const val ACTION_START = "com.example.crowdmap.action.START_TRACKING"
        private const val ACTION_STOP  = "com.example.crowdmap.action.STOP_TRACKING"
        private const val CHANNEL_ID = "crowdmap_tracking"
        private const val NOTIFICATION_ID = 42

        fun start(context: Context) {
            val intent = Intent(context, TrackingService::class.java)
                .setAction(ACTION_START)
            ContextCompat.startForegroundService(context, intent)
        }

        fun stop(context: Context) {
            context.startService(
                Intent(context, TrackingService::class.java).setAction(ACTION_STOP)
            )
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                TrackingCore.stopTracking()
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf()
            }
            // ACTION_START 또는 프로세스 강제 종료 후 START_STICKY 재시작(null intent)
            else -> {
                TrackingCore.init(this)
                startForeground(NOTIFICATION_ID, buildNotification())
                TrackingCore.startTracking()
            }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        // 시스템이 서비스를 강제 종료하는 경로에서도 수집을 정리 (idempotent)
        TrackingCore.stopTracking()
        super.onDestroy()
    }

    private fun buildNotification(): Notification {
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(
            NotificationChannel(
                CHANNEL_ID, "위치 공유",
                NotificationManager.IMPORTANCE_LOW   // 무음 — 상태 표시만
            )
        )

        val contentIntent = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE
        )

        return Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("CrowdMap 위치 공유 중")
            .setContentText("주변 혼잡도 측정을 위해 위치를 공유하고 있습니다")
            .setSmallIcon(R.drawable.ic_launcher_foreground)
            .setContentIntent(contentIntent)
            .setOngoing(true)
            .build()
    }
}
