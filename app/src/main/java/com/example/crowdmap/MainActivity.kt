package com.example.crowdmap

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.Button
import android.widget.TextView
import android.widget.Toast
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

    // ── 지도 탭 시 표시되는 원/마커 (1개만 유지) ──────────────────────────
    private var selectedCircle: com.google.android.gms.maps.model.Circle? = null
    private var selectedMarker: com.google.android.gms.maps.model.Marker? = null

    // ── 위치 추적 중 표시되는 원/마커 (1개만 유지) ────────────────────────
    private var trackingCircle: com.google.android.gms.maps.model.Circle? = null
    private var trackingMarker: com.google.android.gms.maps.model.Marker? = null

    companion object {
        const val LOCATION_PERMISSION_REQUEST = 1001
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus     = findViewById(R.id.tvStatus)
        tvLocation   = findViewById(R.id.tvLocation)
        tvCongestion = findViewById(R.id.tvCongestion)
        btnConnect   = findViewById(R.id.btnConnect)
        btnStart     = findViewById(R.id.btnStart)

        val mapFragment = supportFragmentManager
            .findFragmentById(R.id.map) as SupportMapFragment
        mapFragment.getMapAsync(this)

        locationManager = CrowdLocationManager(this)
        serverClient    = ServerClient()

        btnConnect.setOnClickListener { connectToServer() }
        btnStart.setOnClickListener   { toggleTracking() }

        requestLocationPermission()
    }

    override fun onMapReady(map: GoogleMap) {
        googleMap = map

        val fallback = LatLng(35.8890, 128.6100)  // 경북대학교
        googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(fallback, 15f))

        if (ContextCompat.checkSelfPermission(
                this, Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            googleMap.isMyLocationEnabled = true
            locationManager.getLastKnownLocation { lat, lng ->
                googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), 15f))
            }
        }

        googleMap.setOnMapClickListener { latLng ->
            if (!serverClient.isConnected()) {
                Toast.makeText(this, "먼저 서버에 연결하세요", Toast.LENGTH_SHORT).show()
                return@setOnMapClickListener
            }
            lifecycleScope.launch {
                val congestion = serverClient.getCongestion(latLng.latitude, latLng.longitude)
                if (congestion == null) {
                    val msg = if (!serverClient.isConnected()) "서버 연결이 끊겼습니다. 다시 연결하세요"
                    else "혼잡도 데이터를 받지 못했습니다"
                    Toast.makeText(this@MainActivity, msg, Toast.LENGTH_SHORT).show()
                    if (!serverClient.isConnected()) tvStatus.text = "❌ 연결 끊김"
                    return@launch
                }

                // ── 이전 탭 원/마커 제거 후 새로 그리기 ──────────────────
                selectedCircle?.remove()
                selectedMarker?.remove()

                selectedCircle = googleMap.addCircle(
                    CircleOptions()
                        .center(latLng)
                        .radius(200.0)
                        .fillColor((congestion.color() and 0x00FFFFFF) or 0x4D000000)
                        .strokeColor(congestion.color())
                        .strokeWidth(5f)
                )
                selectedMarker = googleMap.addMarker(
                    MarkerOptions()
                        .position(latLng)
                        .title("혼잡도: ${congestion.levelKorean()} (${(congestion.ratio * 100).toInt()}%)")
                )
            }
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

            googleMap.animateCamera(
                CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), 15f)
            )

            lifecycleScope.launch {
                val result = serverClient.sendLocation(userId, lat, lng)
                if (result == null && !serverClient.isConnected()) {
                    tvStatus.text = "❌ 연결 끊김"
                    stopTracking()
                }
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

    private fun updateCongestionUI(data: CongestionData, lat: Double, lng: Double) {
        tvCongestion.text = "혼잡도: ${data.levelKorean()} (${(data.ratio * 100).toInt()}%)"
        tvCongestion.setTextColor(data.color())

        val position = LatLng(lat, lng)

        // ── 이전 추적 원/마커 제거 후 새로 그리기 (중복 방지) ────────────
        trackingCircle?.remove()
        trackingMarker?.remove()

        trackingCircle = googleMap.addCircle(
            CircleOptions()
                .center(position)
                .radius(200.0)
                .fillColor((data.color() and 0x00FFFFFF) or 0x4D000000)
                .strokeColor(data.color())
                .strokeWidth(3f)
        )
        trackingMarker = googleMap.addMarker(
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