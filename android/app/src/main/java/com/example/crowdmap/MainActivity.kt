package com.example.crowdmap

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.example.crowdmap.location.CrowdLocationManager
import com.example.crowdmap.model.CongestionData
import com.example.crowdmap.network.ServerClient
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var locationManager: CrowdLocationManager
    private lateinit var serverClient: ServerClient

    private lateinit var tvStatus: TextView
    private lateinit var tvLocation: TextView
    private lateinit var tvCongestion: TextView
    private lateinit var btnConnect: Button
    private lateinit var btnStart: Button

    private var userId = 1001  // 임시 사용자 ID
    private var isTracking = false

    companion object {
        const val LOCATION_PERMISSION_REQUEST = 1001
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        // 뷰 초기화
        tvStatus     = findViewById(R.id.tvStatus)
        tvLocation   = findViewById(R.id.tvLocation)
        tvCongestion = findViewById(R.id.tvCongestion)
        btnConnect   = findViewById(R.id.btnConnect)
        btnStart     = findViewById(R.id.btnStart)

        // 매니저 초기화
        locationManager = CrowdLocationManager(this)
        serverClient    = ServerClient()

        // 버튼 클릭
        btnConnect.setOnClickListener { connectToServer() }
        btnStart.setOnClickListener   { toggleTracking() }

        // 위치 권한 요청
        requestLocationPermission()
    }

    // 서버 연결
    private fun connectToServer() {
        tvStatus.text = "연결 중..."
        lifecycleScope.launch {
            val connected = serverClient.connect()
            tvStatus.text = if (connected) "서버 연결됨" else "연결 실패"
        }
    }

    // 위치 추적 시작/중지 토글
    private fun toggleTracking() {
        if (!isTracking) {
            startTracking()
        } else {
            stopTracking()
        }
    }

    private fun startTracking() {
        if (!serverClient.isConnected()) {
            tvStatus.text = "먼저 서버에 연결하세요"
            return
        }
        isTracking = true
        btnStart.text = "추적 중지"
        tvStatus.text = "위치 추적 중..."

        locationManager.startLocationUpdates { lat, lng ->
            tvLocation.text = "위치: $lat, $lng"

            // 서버로 위치 전송
            lifecycleScope.launch {
                val result = serverClient.sendLocation(userId, lat, lng)
                result?.let { updateCongestionUI(it) }
            }
        }
    }

    private fun stopTracking() {
        isTracking = false
        btnStart.text = "추적 시작"
        tvStatus.text = "추적 중지됨"
        locationManager.stopLocationUpdates()
    }

    // 혼잡도 UI 업데이트
    private fun updateCongestionUI(data: CongestionData) {
        tvCongestion.text = "혼잡도: ${data.levelKorean()} (${(data.ratio * 100).toInt()}%)"
        tvCongestion.setTextColor(data.color())
    }

    // 위치 권한 요청
    private fun requestLocationPermission() {
        if (ContextCompat.checkSelfPermission(
                this, Manifest.permission.ACCESS_FINE_LOCATION
            ) != PackageManager.PERMISSION_GRANTED
        ) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(
                    Manifest.permission.ACCESS_FINE_LOCATION,
                    Manifest.permission.ACCESS_COARSE_LOCATION
                ),
                LOCATION_PERMISSION_REQUEST
            )
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        stopTracking()
        serverClient.disconnect()
    }
}