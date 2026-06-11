package com.example.crowdmap.location

import android.annotation.SuppressLint
import android.content.Context
import android.location.Location
import android.os.Looper
import com.google.android.gms.location.FusedLocationProviderClient
import com.google.android.gms.location.LocationCallback
import com.google.android.gms.location.LocationRequest
import com.google.android.gms.location.LocationResult
import com.google.android.gms.location.LocationServices
import com.google.android.gms.location.Priority

class CrowdLocationManager(private val context: Context) {

    private val fusedLocationClient: FusedLocationProviderClient =
        LocationServices.getFusedLocationProviderClient(context)

    private var locationCallback: LocationCallback? = null

    // 마지막 위치 저장 (중복 전송 방지)
    private var lastLocation: Location? = null

    companion object {
        const val MIN_DISTANCE_METERS = 10f  // 10m 이상 이동 시 업데이트
        const val UPDATE_INTERVAL_MS = 5000L // 5초마다 업데이트

        // OS가 위치 fix를 하드웨어 FIFO에 모았다가 한 번에 전달하도록 허용하는
        // 최대 지연. 매 fix마다 앱 프로세스를 깨우지 않으므로 수집 비용이 크게
        // 줄어든다. 전송 배치 주기(10초)와 동일하게 맞춰 서버로 나가는 데이터의
        // 타이밍에는 영향이 없다.
        const val MAX_BATCH_DELAY_MS = 10_000L
    }

    @SuppressLint("MissingPermission")
    fun startLocationUpdates(onLocationChanged: (Double, Double) -> Unit) {

        // 혼잡도 집계는 서버의 0.001도(약 100m) 격자 단위로 이뤄지므로
        // GPS 정밀 측위 대신 Wi-Fi/기지국 기반 BALANCED 모드로 충분하다.
        // HIGH_ACCURACY 대비 전력 소모가 수 배 낮아 군중 속 장시간 추적에 유리.
        val locationRequest = LocationRequest.Builder(
            Priority.PRIORITY_BALANCED_POWER_ACCURACY,
            UPDATE_INTERVAL_MS
        )
            .setMinUpdateDistanceMeters(MIN_DISTANCE_METERS)
            .setMaxUpdateDelayMillis(MAX_BATCH_DELAY_MS)
            .build()

        locationCallback = object : LocationCallback() {
            override fun onLocationResult(result: LocationResult) {
                // 배치 전달이 허용되면 한 콜백에 여러 fix가 담겨 올 수 있다.
                // 마지막 것만 쓰면 이동 경로가 누락되므로 전부 처리한다.
                for (location in result.locations) {
                    if (shouldUpdate(location)) {
                        lastLocation = location
                        onLocationChanged(location.latitude, location.longitude)
                    }
                }
            }
        }

        fusedLocationClient.requestLocationUpdates(
            locationRequest,
            locationCallback!!,
            Looper.getMainLooper()
        )

        println("[Location] 위치 수집 시작")
    }

    // 이전 위치와 비교해서 MIN_DISTANCE_METERS 이상 이동했는지 확인
    private fun shouldUpdate(newLocation: Location): Boolean {
        val last = lastLocation ?: return true
        return last.distanceTo(newLocation) >= MIN_DISTANCE_METERS
    }

    @SuppressLint("MissingPermission")
    fun getLastKnownLocation(onResult: (Double, Double) -> Unit) {
        fusedLocationClient.lastLocation.addOnSuccessListener { location ->
            if (location != null) {
                onResult(location.latitude, location.longitude)
            }
        }
    }

    fun stopLocationUpdates() {
        locationCallback?.let {
            fusedLocationClient.removeLocationUpdates(it)
            println("[Location] 위치 수집 중지")
        }
    }
}
