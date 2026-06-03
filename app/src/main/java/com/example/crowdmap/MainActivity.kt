package com.example.crowdmap

import android.Manifest
import android.content.pm.PackageManager
import android.location.Geocoder
import android.os.Build
import android.os.Bundle
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.example.crowdmap.ble.BleScanner
import com.example.crowdmap.location.CrowdLocationManager
import com.example.crowdmap.location.LocationRepository
import com.example.crowdmap.model.CongestionData
import com.example.crowdmap.network.ServerClient
import com.google.android.gms.maps.CameraUpdateFactory
import com.google.android.gms.maps.GoogleMap
import com.google.android.gms.maps.OnMapReadyCallback
import com.google.android.gms.maps.SupportMapFragment
import com.google.android.gms.maps.model.Circle
import com.google.android.gms.maps.model.CircleOptions
import com.google.android.gms.maps.model.LatLng
import com.google.android.gms.maps.model.Marker
import com.google.android.gms.maps.model.MarkerOptions
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale

class MainActivity : AppCompatActivity(), OnMapReadyCallback {

    private lateinit var locationManager: CrowdLocationManager
    private lateinit var serverClient: ServerClient
    private lateinit var locationRepository: LocationRepository
    private lateinit var googleMap: GoogleMap
    private lateinit var bleScanner: BleScanner

    private lateinit var tvStatus: TextView
    private lateinit var tvLocation: TextView
    private lateinit var tvCongestion: TextView
    private lateinit var btnConnect: Button
    private lateinit var btnStart: Button
    private lateinit var etSearch: EditText      // ← POI 검색창
    private lateinit var btnSearch: Button       // ← POI 검색 버튼

    private var userId = 1001
    private var isTracking = false
    private var currentLatLng: LatLng? = null

    // ── 지도 탭 시 표시되는 원/마커 (1개만 유지) ──────────────────────────
    private var selectedCircle: Circle? = null
    private var selectedMarker: Marker? = null

    // ── 위치 추적 중 표시되는 원/마커 (1개만 유지) ────────────────────────
    private var trackingCircle: Circle? = null
    private var trackingMarker: Marker? = null

    companion object {
        const val LOCATION_PERMISSION_REQUEST = 1001
        const val BLUETOOTH_PERMISSION_REQUEST = 1002
        const val DEFAULT_ZOOM = 15f        // 탭/검색 시 확대될 기본 줌 레벨
        const val CIRCLE_RADIUS_M = 200.0   // 원 반지름 고정 200m
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        tvStatus     = findViewById(R.id.tvStatus)
        tvLocation   = findViewById(R.id.tvLocation)
        tvCongestion = findViewById(R.id.tvCongestion)
        btnConnect   = findViewById(R.id.btnConnect)
        btnStart     = findViewById(R.id.btnStart)
        etSearch     = findViewById(R.id.etSearch)
        btnSearch    = findViewById(R.id.btnSearch)

        val mapFragment = supportFragmentManager
            .findFragmentById(R.id.map) as SupportMapFragment
        mapFragment.getMapAsync(this)

        locationManager    = CrowdLocationManager(this)
        serverClient       = ServerClient()
        bleScanner         = BleScanner(this)
        locationRepository = LocationRepository(serverClient, lifecycleScope, userId, bleScanner)

        btnConnect.setOnClickListener { connectToServer() }
        btnStart.setOnClickListener   { toggleTracking() }

        // ── POI 검색 버튼 클릭 ────────────────────────────────────────────
        btnSearch.setOnClickListener { searchPlace() }

        // ── 키보드 검색(엔터) 버튼으로도 동작 ────────────────────────────
        etSearch.setOnEditorActionListener { _, actionId, _ ->
            if (actionId == EditorInfo.IME_ACTION_SEARCH) {
                searchPlace()
                true
            } else false
        }

        requestLocationPermission()
        requestBluetoothPermission()
    }

    // ── POI 검색 ─────────────────────────────────────────────────────────
    // Android 내장 Geocoder로 장소 이름 → 좌표 변환 (별도 API 키 불필요)
    // 검색 결과 없을 때만 "대구" 붙여서 재시도 (대구 지역 우선)
    // 검색 후 DEFAULT_ZOOM(15f)으로 확대 + 서버 연결 시 혼잡도 자동 조회
    private fun searchPlace() {
        val query = etSearch.text.toString().trim()
        if (query.isEmpty()) {
            Toast.makeText(this, "검색어를 입력하세요", Toast.LENGTH_SHORT).show()
            return
        }

        // 키보드 숨기기
        val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
        imm.hideSoftInputFromWindow(etSearch.windowToken, 0)

        lifecycleScope.launch {
            // Geocoder는 블로킹 I/O → IO 스레드에서 실행
            val latLng = withContext(Dispatchers.IO) {
                try {
                    val geocoder = Geocoder(this@MainActivity, Locale.KOREAN)

                    // 먼저 검색어 그대로 시도 → 결과 없을 때만 "대구" 붙여서 재시도
                    @Suppress("DEPRECATION")
                    val results = geocoder.getFromLocationName(query, 1)
                    if (!results.isNullOrEmpty()) {
                        LatLng(results[0].latitude, results[0].longitude)
                    } else {
                        @Suppress("DEPRECATION")
                        val fallback = geocoder.getFromLocationName("$query 대구", 1)
                        if (!fallback.isNullOrEmpty()) {
                            LatLng(fallback[0].latitude, fallback[0].longitude)
                        } else null
                    }
                } catch (e: Exception) {
                    null
                }
            }

            if (latLng == null) {
                Toast.makeText(this@MainActivity,
                    "\"$query\" 장소를 찾을 수 없습니다", Toast.LENGTH_SHORT).show()
                return@launch
            }

            // 검색한 위치로 DEFAULT_ZOOM(15f)으로 확대 이동
            googleMap.animateCamera(
                CameraUpdateFactory.newLatLngZoom(latLng, DEFAULT_ZOOM))

            if (serverClient.isConnected()) {
                val congestion = serverClient.getCongestion(latLng.latitude, latLng.longitude)
                if (congestion != null) {
                    tvLocation.text = "위치: ${String.format("%.6f", latLng.latitude)}, " +
                            "${String.format("%.6f", latLng.longitude)}"
                    tvCongestion.text =
                        "혼잡도: ${congestion.levelKorean()} (${(congestion.ratio * 100).toInt()}%)"
                    tvCongestion.setTextColor(congestion.color())

                    // ── 이전 탭 원/마커 제거 후 새로 그리기 ──────────────
                    selectedCircle?.remove()
                    selectedMarker?.remove()

                    selectedCircle = googleMap.addCircle(
                        CircleOptions()
                            .center(latLng)
                            .radius(CIRCLE_RADIUS_M)  // 고정 200m
                            .fillColor((congestion.color() and 0x00FFFFFF) or 0x4D000000)
                            .strokeColor(congestion.color())
                            .strokeWidth(5f)
                    )
                    selectedMarker = googleMap.addMarker(
                        MarkerOptions()
                            .position(latLng)
                            .title("$query — 혼잡도: ${congestion.levelKorean()} " +
                                    "(${(congestion.ratio * 100).toInt()}%)")
                    )
                    selectedMarker?.showInfoWindow()
                } else {
                    Toast.makeText(this@MainActivity,
                        "혼잡도 데이터를 받지 못했습니다", Toast.LENGTH_SHORT).show()
                }
            } else {
                // 서버 미연결이면 지도 이동만 수행
                Toast.makeText(
                    this@MainActivity,
                    "\"$query\" 위치로 이동했습니다. 혼잡도 조회는 서버 연결 후 가능합니다",
                    Toast.LENGTH_LONG
                ).show()
            }
        }
    }

    override fun onMapReady(map: GoogleMap) {
        googleMap = map

        val fallback = LatLng(35.8890, 128.6100)  // 경북대학교
        googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(fallback, DEFAULT_ZOOM))

        if (ContextCompat.checkSelfPermission(
                this, Manifest.permission.ACCESS_FINE_LOCATION
            ) == PackageManager.PERMISSION_GRANTED
        ) {
            googleMap.isMyLocationEnabled = true
            locationManager.getLastKnownLocation { lat, lng ->
                googleMap.moveCamera(
                    CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), DEFAULT_ZOOM))
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

                // ── 클릭한 위치 정보 상단에 반영 ──────────────────────
                val lat = String.format("%.6f", latLng.latitude)
                val lng = String.format("%.6f", latLng.longitude)
                tvLocation.text = "위치: $lat, $lng"
                tvCongestion.text =
                    "혼잡도: ${congestion.levelKorean()} (${(congestion.ratio * 100).toInt()}%)"
                tvCongestion.setTextColor(congestion.color())

                // ── 이전 탭 원/마커 제거 후 새로 그리기 ──────────────
                selectedCircle?.remove()
                selectedMarker?.remove()

                // 탭한 위치로 DEFAULT_ZOOM(15f)으로 확대 이동
                // 이미 15f 이상으로 보고 있으면 현재 줌 유지
                val currentZoom = googleMap.cameraPosition.zoom
                val targetZoom = maxOf(currentZoom, DEFAULT_ZOOM)
                googleMap.animateCamera(
                    CameraUpdateFactory.newLatLngZoom(latLng, targetZoom))

                selectedCircle = googleMap.addCircle(
                    CircleOptions()
                        .center(latLng)
                        .radius(CIRCLE_RADIUS_M)  // 고정 200m
                        .fillColor((congestion.color() and 0x00FFFFFF) or 0x4D000000)
                        .strokeColor(congestion.color())
                        .strokeWidth(5f)
                )
                selectedMarker = googleMap.addMarker(
                    MarkerOptions()
                        .position(latLng)
                        .title("혼잡도: ${congestion.levelKorean()} " +
                                "(${(congestion.ratio * 100).toInt()}%)")
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

        // BLE 스캔 시작 (가능한 경우)
        if (bleScanner.isAvailable()) {
            bleScanner.startScan()
        }

        locationManager.startLocationUpdates { lat, lng ->
            currentLatLng = LatLng(lat, lng)
            tvLocation.text = "위치: $lat, $lng"

            googleMap.animateCamera(
                CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), DEFAULT_ZOOM)
            )

            // 1. 고성능 배치 전송을 위해 Repository로 위임 (Non-blocking)
            val location = android.location.Location("").apply {
                latitude = lat
                longitude = lng
            }
            locationRepository.onLocationReceived(location)

            // 2. 실시간 혼잡도 UI 업데이트를 위해 1회성 조회 수행 (선택적)
            lifecycleScope.launch {
                val result = serverClient.getCongestion(lat, lng)
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
        bleScanner.stopScan()
    }

    private fun updateCongestionUI(data: CongestionData, lat: Double, lng: Double) {
        tvCongestion.text =
            "혼잡도: ${data.levelKorean()} (${(data.ratio * 100).toInt()}%)"
        tvCongestion.setTextColor(data.color())

        val position = LatLng(lat, lng)

        // ── 이전 추적 원/마커 제거 후 새로 그리기 (중복 방지) ────────────
        trackingCircle?.remove()
        trackingMarker?.remove()

        trackingCircle = googleMap.addCircle(
            CircleOptions()
                .center(position)
                .radius(CIRCLE_RADIUS_M)  // 고정 200m
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

    private fun requestBluetoothPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            if (ContextCompat.checkSelfPermission(
                    this, Manifest.permission.BLUETOOTH_SCAN
                ) != PackageManager.PERMISSION_GRANTED
            ) {
                ActivityCompat.requestPermissions(
                    this,
                    arrayOf(Manifest.permission.BLUETOOTH_SCAN),
                    BLUETOOTH_PERMISSION_REQUEST
                )
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        // 메모리 해제: 원/마커 명시적 제거
        selectedCircle?.remove(); selectedCircle = null
        selectedMarker?.remove(); selectedMarker = null
        trackingCircle?.remove(); trackingCircle = null
        trackingMarker?.remove(); trackingMarker = null
        stopTracking()
        serverClient.disconnect()
    }
}