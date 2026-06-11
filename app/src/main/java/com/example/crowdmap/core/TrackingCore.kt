package com.example.crowdmap.core

import android.content.Context
import android.location.Location
import com.example.crowdmap.ble.BleScanner
import com.example.crowdmap.location.CrowdLocationManager
import com.example.crowdmap.location.LocationRepository
import com.example.crowdmap.model.CongestionData
import com.example.crowdmap.network.ServerClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * [TrackingCore]
 * 위치 수집·전송 파이프라인(ServerClient, LocationRepository, BleScanner,
 * CrowdLocationManager)을 소유하는 앱 전역 싱글톤.
 *
 * 기존에는 MainActivity가 이 객체들을 직접 소유해서
 *  - 화면 회전 시 소켓/재시도 버퍼/추적 상태가 전부 초기화되고
 *  - 화면을 끄면(백그라운드) 수집이 멈춰 군중 데이터가 끊겼다.
 * 군중 앱의 핵심 데이터원은 "주머니 속 폰"이므로 파이프라인 수명을
 * Activity가 아닌 앱 프로세스(+포그라운드 서비스)에 묶는다.
 *
 * UI(MainActivity)는 아래 StateFlow를 구독해 상태를 그리기만 한다.
 */
object TrackingCore {

    private const val PREFS_NAME = "crowdmap"
    private const val KEY_USER_ID = "user_id"

    // Activity 생명주기와 무관하게 살아 있는 파이프라인 전용 스코프
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private lateinit var serverClient: ServerClient
    private lateinit var bleScanner: BleScanner
    private lateinit var locationManager: CrowdLocationManager
    private lateinit var repository: LocationRepository

    @Volatile
    private var initialized = false

    // 자기 위치 혼잡도 조회 잡: 최신 위치 1건만 의미가 있으므로
    // 새 위치가 오면 이전 조회를 취소한다(conflation) — 서버가 느릴 때
    // stale 조회가 소켓 락 앞에 줄을 서는 것을 방지.
    private var congestionJob: Job? = null

    // ── UI가 구독하는 상태 ──────────────────────────────────────────
    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    private val _isTracking = MutableStateFlow(false)
    val isTracking: StateFlow<Boolean> = _isTracking.asStateFlow()

    private val _location = MutableStateFlow<Pair<Double, Double>?>(null)
    val location: StateFlow<Pair<Double, Double>?> = _location.asStateFlow()

    private val _congestion = MutableStateFlow<CongestionData?>(null)
    val congestion: StateFlow<CongestionData?> = _congestion.asStateFlow()

    /** 앱 어디서든(Activity/Service) 먼저 호출. 중복 호출은 무시된다. */
    @Synchronized
    fun init(context: Context) {
        if (initialized) return
        val app = context.applicationContext

        serverClient    = ServerClient()
        bleScanner      = BleScanner(app)
        locationManager = CrowdLocationManager(app)
        repository      = LocationRepository(serverClient, scope, loadUserId(app), bleScanner)

        initialized = true
    }

    // 단말마다 고유한 userId를 한 번 생성해 영구 저장한다.
    // 모든 단말이 같은 값을 쓰면 서버에서 사용자 구분이 불가능해
    // 대규모 동시 접속 시 집계가 왜곡된다. (0은 조회 전용으로 예약됨)
    private fun loadUserId(context: Context): Int {
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        var id = prefs.getInt(KEY_USER_ID, 0)
        if (id == 0) {
            id = (1..Int.MAX_VALUE).random()
            prefs.edit().putInt(KEY_USER_ID, id).apply()
        }
        return id
    }

    suspend fun connect(): Boolean {
        val ok = serverClient.connect()
        _connected.value = ok
        return ok
    }

    fun isConnected(): Boolean = serverClient.isConnected()

    /** TrackingService(포그라운드)에서 호출된다. */
    fun startTracking() {
        if (_isTracking.value) return
        _isTracking.value = true

        repository.setTracking(true)
        if (bleScanner.isAvailable()) {
            bleScanner.startScan()
        }
        locationManager.startLocationUpdates { lat, lng -> onLocation(lat, lng) }
    }

    fun stopTracking() {
        if (!_isTracking.value) return
        _isTracking.value = false

        repository.setTracking(false)
        locationManager.stopLocationUpdates()
        bleScanner.stopScan()
        congestionJob?.cancel()
    }

    private fun onLocation(lat: Double, lng: Double) {
        _location.value = lat to lng

        val location = Location("").apply {
            latitude = lat
            longitude = lng
        }
        repository.onLocationReceived(location)

        // 자기 위치 혼잡도 조회 (UI 표시용) — 이전 조회는 취소하고 최신만 유지.
        // applyIntervalHint=true: 응답의 권장 주기를 배치 전송 주기에 반영한다.
        congestionJob?.cancel()
        congestionJob = scope.launch {
            val result = serverClient.getCongestion(lat, lng, applyIntervalHint = true)
            if (result != null) _congestion.value = result
            _connected.value = serverClient.isConnected()
        }
    }

    /** 지도 탭 등 임의 지점 조회 (주기 힌트는 반영하지 않음) */
    suspend fun queryCongestion(lat: Double, lng: Double): CongestionData? {
        val result = serverClient.getCongestion(lat, lng)
        _connected.value = serverClient.isConnected()
        return result
    }

    /** 지도 초기 카메라 위치용 passthrough */
    fun getLastKnownLocation(onResult: (Double, Double) -> Unit) {
        locationManager.getLastKnownLocation(onResult)
    }

    /** 추적 중이 아닐 때만 연결을 정리한다 (앱 종료 시 서버 fd 반납). */
    fun shutdownIfIdle() {
        if (!_isTracking.value) {
            serverClient.disconnect()
            _connected.value = false
        }
    }
}
