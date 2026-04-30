package com.example.crowdmap

import android.Manifest
import android.content.pm.PackageManager
import android.graphics.Color
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
import com.google.android.gms.maps.CameraUpdateFactory
import com.google.android.gms.maps.GoogleMap
import com.google.android.gms.maps.OnMapReadyCallback
import com.google.android.gms.maps.SupportMapFragment
import com.google.android.gms.maps.model.CircleOptions
import com.google.android.gms.maps.model.LatLng
import com.google.android.gms.maps.model.MarkerOptions
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity(), OnMapReadyCallback {

    private lateinit var locationManager: CrowdLocationManager
    private lateinit var serverClient: ServerClient
    private lateinit var googleMap: GoogleMap

    private lateinit var tvStatus: TextView
    private lateinit var tvLocation: TextView
    private lateinit var tvCongestion: TextView
    private lateinit var btnConnect: Button
    private lateinit var btnStart: Button

    private var userId = 1001
    private var isTracking = false
    private var currentLatLng: LatLng? = null

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

        // 지도 초기화
        val mapFragment = supportFragmentManager
            .findFragmentById(R.id.map) as SupportMapFragment
        mapFragment.getMapAsync(this)

        // 매니저 초기화
        locationManager = CrowdLocationManager(this)
        serverClient    = ServerClient()

        btnConnect.setOnClickListener { connectToServer() }
        btnStart.setOnClickListener   { toggleTracking() }

        requestLocationPermission()
    }

    // 지도 준비 완료 콜백
    override fun onMapReady(map: GoogleMap) {
        googleMap = map

        // 대구로 초기 카메라 이동
        val daegu = LatLng(35.8714, 128.6014)
        googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(daegu, 14f))

        // 내 위치 버튼 표시
        if (ContextCompat.checkSelfPermission(
                this, Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            googleMap.isMyLocationEnabled = true
        }
    }

    private fun connectToServer() {
        tvStatus.text = "연결 중..."
        lifecycleScope.launch {
            val connected = serverClient.connect()
            tvStatus.text = if (connected) "✅ 서버 연결됨" else "❌ 연결 실패"
        }
    }

    private fun toggleTracking() {
        if (!isTracking) startTracking() else stopTracking()
    }

    private fun startTracking() {
        if (!serverClient.isConnected()) {
            tvStatus.text = "먼저 서버에 연결하세요"
            return
        }
        isTracking = true
        btnStart.text = "추적 중지"
        tvStatus.text = "📍 위치 추적 중..."

        locationManager.startLocationUpdates { lat, lng ->
            currentLatLng = LatLng(lat, lng)
            tvLocation.text = "위치: $lat, $lng"

            // 지도 카메라 이동
            googleMap.animateCamera(
                CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), 15f)
            )

            // 서버로 위치 전송
            lifecycleScope.launch {
                val result = serverClient.sendLocation(userId, lat, lng)
                result?.let { updateCongestionUI(it, lat, lng) }
            }
        }
    }

    private fun stopTracking() {
        isTracking = false
        btnStart.text = "추적 시작"
        tvStatus.text = "추적 중지됨"
        locationManager.stopLocationUpdates()
    }

    // 혼잡도 UI + 지도에 원 표시
    private fun updateCongestionUI(data: CongestionData, lat: Double, lng: Double) {
        tvCongestion.text = "혼잡도: ${data.levelKorean()} (${(data.ratio * 100).toInt()}%)"
        tvCongestion.setTextColor(data.color())

        // 지도에 혼잡도 원 그리기
        val position = LatLng(lat, lng)
        googleMap.addCircle(
            CircleOptions()
                .center(position)
                .radius(200.0)           // 반경 200m
                .fillColor(data.color() and 0x80FFFFFF.toInt())  // 반투명
                .strokeColor(data.color())
                .strokeWidth(3f)
        )

        // 마커 추가
        googleMap.addMarker(
            MarkerOptions()
                .position(position)
                .title("혼잡도: ${data.levelKorean()}")
                .snippet("${(data.ratio * 100).toInt()}%")
        )
    }

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