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
import com.example.crowdmap.core.TrackingCore
import com.example.crowdmap.model.CongestionData
import com.example.crowdmap.service.TrackingService
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

/**
 * 지도/버튼 UI 전담 Activity.
 *
 * 수집·전송 파이프라인은 TrackingCore(앱 전역 싱글톤)가 소유하고,
 * 백그라운드 생존은 TrackingService(포그라운드 서비스)가 보장한다.
 * 이 Activity는 TrackingCore의 StateFlow를 구독해 그리기만 하므로
 * 화면 회전/백그라운드 전환에도 추적과 소켓 연결이 끊기지 않는다.
 */
class MainActivity : AppCompatActivity(), OnMapReadyCallback {

    private lateinit var googleMap: GoogleMap

    private lateinit var tvStatus: TextView
    private lateinit var tvLocation: TextView
    private lateinit var tvCongestion: TextView
    private lateinit var btnConnect: Button
    private lateinit var btnStart: Button
    private lateinit var etSearch: EditText      // ← POI 검색창
    private lateinit var btnSearch: Button       // ← POI 검색 버튼

    // ── 지도 탭 시 표시되는 원/마커 (1개만 유지) ──────────────────────────
    private var selectedCircle: Circle? = null
    private var selectedMarker: Marker? = null

    // ── 위치 추적 중 표시되는 원/마커 (1개만 유지) ────────────────────────
    private var trackingCircle: Circle? = null
    private var trackingMarker: Marker? = null

    companion object {
        const val LOCATION_PERMISSION_REQUEST = 1001
        const val BLUETOOTH_PERMISSION_REQUEST = 1002
        const val NOTIFICATION_PERMISSION_REQUEST = 1003
        const val DEFAULT_ZOOM = 15f
        const val CIRCLE_RADIUS_M = 200.0
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        TrackingCore.init(this)

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
        requestNotificationPermission()

        observeTrackingState()
    }

    // TrackingCore의 상태를 구독해 UI에 반영한다.
    // (회전 후 재생성돼도 StateFlow의 현재 값으로 즉시 복원된다)
    private fun observeTrackingState() {
        lifecycleScope.launch {
            TrackingCore.isTracking.collect { tracking ->
                btnStart.text = if (tracking) "추적 중지" else "추적 시작"
                if (tracking) {
                    tvStatus.text = "📍 위치 추적 중..."
                } else if (TrackingCore.connected.value) {
                    tvStatus.text = "✅ 서버 연결됨"
                }
            }
        }

        lifecycleScope.launch {
            TrackingCore.connected.collect { ok ->
                if (!ok && TrackingCore.isTracking.value) {
                    // 자동 재연결(백오프)이 백그라운드에서 계속 시도된다
                    tvStatus.text = "❌ 연결 끊김 (자동 재연결 중...)"
                }
            }
        }

        lifecycleScope.launch {
            TrackingCore.location.collect { loc ->
                if (loc == null) return@collect
                tvLocation.text = "위치: ${loc.first}, ${loc.second}"
                if (TrackingCore.isTracking.value && ::googleMap.isInitialized) {
                    googleMap.animateCamera(
                        CameraUpdateFactory.newLatLngZoom(LatLng(loc.first, loc.second), 15f)
                    )
                }
            }
        }

        lifecycleScope.launch {
            TrackingCore.congestion.collect { data ->
                val loc = TrackingCore.location.value
                if (data != null && loc != null && TrackingCore.isTracking.value) {
                    updateCongestionUI(data, loc.first, loc.second)
                }
            }
        }
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

            if (TrackingCore.isConnected()) {
                val congestion = TrackingCore.queryCongestion(latLng.latitude, latLng.longitude)
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
            TrackingCore.getLastKnownLocation { lat, lng ->
                googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), DEFAULT_ZOOM))
            }
        }

        googleMap.setOnMapClickListener { latLng ->
            if (!TrackingCore.isConnected()) {
                Toast.makeText(this, "먼저 서버에 연결하세요", Toast.LENGTH_SHORT).show()
                return@setOnMapClickListener
            }
            lifecycleScope.launch {
                val congestion = TrackingCore.queryCongestion(latLng.latitude, latLng.longitude)
                if (congestion == null) {
                    val msg = if (!TrackingCore.isConnected()) "서버 연결이 끊겼습니다. 다시 연결하세요"
                    else "혼잡도 데이터를 받지 못했습니다"
                    Toast.makeText(this@MainActivity, msg, Toast.LENGTH_SHORT).show()
                    if (!TrackingCore.isConnected()) tvStatus.text = "❌ 연결 끊김"
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
            val connected = TrackingCore.connect()
            tvStatus.text = if (connected) "✅ 서버 연결됨" else "❌ 연결 실패"
        }
    }

    private fun toggleTracking() {
        if (TrackingCore.isTracking.value) {
            TrackingService.stop(this)
            return
        }

        if (!TrackingCore.isConnected()) {
            tvStatus.text = "먼저 서버에 연결하세요"
            return
        }
        // API 34+: 위치 권한 없이 location 타입 포그라운드 서비스를 시작하면
        // SecurityException이 발생하므로 사전 확인
        if (ContextCompat.checkSelfPermission(
                this, Manifest.permission.ACCESS_FINE_LOCATION
            ) != PackageManager.PERMISSION_GRANTED
        ) {
            Toast.makeText(this, "위치 권한이 필요합니다", Toast.LENGTH_SHORT).show()
            requestLocationPermission()
            return
        }

        // 포그라운드 서비스가 TrackingCore.startTracking()을 호출하고
        // 화면이 꺼져도 수집을 유지한다
        TrackingService.start(this)
    }

    private fun updateCongestionUI(data: CongestionData, lat: Double, lng: Double) {
        tvCongestion.text =
            "혼잡도: ${data.levelKorean()} (${(data.ratio * 100).toInt()}%)"
        tvCongestion.setTextColor(data.color())

        if (!::googleMap.isInitialized) return
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
            // SCAN: 주변 밀도 측정, ADVERTISE: EN방식 임시 ID 광고 송출
            val missing = arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_ADVERTISE
            ).filter {
                ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
            }
            if (missing.isNotEmpty()) {
                ActivityCompat.requestPermissions(
                    this,
                    missing.toTypedArray(),
                    BLUETOOTH_PERMISSION_REQUEST
                )
            }
        }
    }

    // 포그라운드 서비스 알림 표시용 (33+). 거부해도 추적은 동작하며
    // 알림만 보이지 않는다.
    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(
                    this, Manifest.permission.POST_NOTIFICATIONS
                ) != PackageManager.PERMISSION_GRANTED
            ) {
                ActivityCompat.requestPermissions(
                    this,
                    arrayOf(Manifest.permission.POST_NOTIFICATIONS),
                    NOTIFICATION_PERMISSION_REQUEST
                )
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        selectedCircle?.remove(); selectedCircle = null
        selectedMarker?.remove(); selectedMarker = null
        trackingCircle?.remove(); trackingCircle = null
        trackingMarker?.remove(); trackingMarker = null
        TrackingCore.shutdownIfIdle()
    }
}
