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
    }

    @SuppressLint("MissingPermission")
    fun startLocationUpdates(onLocationChanged: (Double, Double) -> Unit) {

        val locationRequest = LocationRequest.Builder(
            Priority.PRIORITY_HIGH_ACCURACY,
            UPDATE_INTERVAL_MS
        )
            .setMinUpdateDistanceMeters(MIN_DISTANCE_METERS)
            .build()

        locationCallback = object : LocationCallback() {
            override fun onLocationResult(result: LocationResult) {
                val location = result.lastLocation ?: return

                // 50m 이상 이동했을 때만 콜백 호출
                if (shouldUpdate(location)) {
                    lastLocation = location
                    println("[Location] 위치 업데이트: ${location.latitude}, ${location.longitude}")
                    onLocationChanged(location.latitude, location.longitude)
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

    // 이전 위치와 비교해서 50m 이상 이동했는지 확인
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