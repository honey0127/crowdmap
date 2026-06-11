package com.example.crowdmap.location

import android.location.Location
import com.example.crowdmap.ble.BleScanner
import com.example.crowdmap.network.ServerClient
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.floor
import kotlin.random.Random

/**
 * [LocationRepository]
 * 고밀도 위치 데이터의 쓰로틀링, 배치 처리, 재시도 로직을 담당하는 핵심 도메인 계층.
 *
 * --- 방법 1: BLE Passive Scanning 보정 ---
 * bleScanner가 제공되면 각 위치 전송 시 BLE 감지 기기 수를 함께 첨부합니다.
 * 전송 형식: "userId,lat,lon,ble=N\n"
 *
 * --- 방법 2: P2P 집계 카운팅 리포트 (서버 트래픽 1/N 감소) ---
 * 주변에 DENSE_ZONE_THRESHOLD 이상의 BLE 기기가 감지되면 (= 밀집 지역),
 * 개별 좌표 배치 대신 경량 존 밀도 리포트를 전송합니다.
 * 전송 형식: "zone=<zoneId>,density=<bleCount>\n"
 * 이 한 줄이 zone 내 N대의 개별 좌표 패킷을 대체하여 서버 트래픽을 대폭 감소시킵니다.
 *
 * --- 대규모 동시 접속 대응 ---
 * 1. 격자 중복 제거: 서버는 0.001도(약 100m) 격자 단위로 집계하므로 같은
 *    격자 안의 좌표를 여러 줄 보내는 것은 정보가 0인데 밀도만 부풀린다.
 *    배치마다 격자당 최신 fix 1줄로 압축한다. (전송량 감소 + 집계 정확도 향상)
 * 2. 정지 사용자 heartbeat: 이동 거리 필터(10m) 때문에 멈춰 있는 사용자는
 *    위치 콜백이 오지 않아 서버 5분 윈도우에서 증발한다. 군중 앱에서는
 *    "멈춰 있는 사람들"이 핵심 신호이므로, 추적 중이면 배치마다 마지막
 *    위치 한 줄로 존재를 갱신한다. (배치당 정확히 1이벤트 = 가중치도 일정)
 * 3. 배치 지터: 모든 단말이 정확히 10초 주기로 전송하면 서버에 도착 시점이
 *    동기화될 수 있어 ±1초 무작위 지터로 부하를 시간축에 분산한다.
 * 4. 좌표 정밀도 절사: 소수 5자리(약 1m)면 100m 격자 집계에 충분하다.
 *    줄당 바이트 수를 줄여 같은 대역폭으로 더 많은 단말을 수용한다.
 */
class LocationRepository(
    private val serverClient: ServerClient,
    private val scope: CoroutineScope,
    private val userId: Int = 1001,
    private val bleScanner: BleScanner? = null
) {
    companion object {
        // 이 수 이상 BLE 기기가 감지되면 밀집 지역으로 판단 → 존 리포트 전송
        private const val DENSE_ZONE_THRESHOLD = 5

        private const val BATCH_INTERVAL_MS = 10_000L
        private const val BATCH_JITTER_MS = 1_000L

        // 서버 SpatialHash::GRID_SIZE(0.001도 ≈ 100m)와 동일한 격자 크기
        private const val DEDUP_GRID = 0.001
    }

    private val locationChannel = Channel<Location>(
        capacity = 200,
        onBufferOverflow = BufferOverflow.DROP_OLDEST
    )

    private val sendBuffer    = StringBuilder(4096)
    private val retryBuffer   = StringBuilder(8192)
    private val MAX_RETRY_BUFFER_SIZE = 1024 * 10

    // 배치 내 격자 중복 제거용 (격자 키 → 해당 격자의 최신 fix)
    private val dedupCells = LinkedHashMap<Long, Location>()

    // 최신 위치 (존 리포트/heartbeat에 사용)
    @Volatile private var lastLat: Double = 0.0
    @Volatile private var lastLng: Double = 0.0
    @Volatile private var hasLocation: Boolean = false

    // 추적 중 여부: 정지 사용자 heartbeat 전송 게이트
    @Volatile private var tracking: Boolean = false

    private val isRunning = AtomicBoolean(false)

    init {
        startBatchWorker()
    }

    fun setTracking(active: Boolean) {
        tracking = active
    }

    fun onLocationReceived(location: Location) {
        lastLat     = location.latitude
        lastLng     = location.longitude
        hasLocation = true
        locationChannel.trySend(location)
    }

    private fun startBatchWorker() {
        if (isRunning.getAndSet(true)) return

        scope.launch(Dispatchers.IO) {
            while (isActive) {
                delay(BATCH_INTERVAL_MS + Random.nextLong(-BATCH_JITTER_MS, BATCH_JITTER_MS))

                sendBuffer.clear()

                if (retryBuffer.isNotEmpty()) {
                    sendBuffer.append(retryBuffer)
                    retryBuffer.clear()
                }

                val bleCount = bleScanner?.getCount() ?: 0

                // ── 방법 2: 밀집 지역 → 존 밀도 리포트로 대체 ──────────────────
                // 주변 기기가 DENSE_ZONE_THRESHOLD 이상이면 개별 좌표 대신
                // 경량 "zone=<id>,density=<N>" 한 줄만 전송해 서버 부하를 낮춘다.
                if (bleCount >= DENSE_ZONE_THRESHOLD && hasLocation) {
                    val zoneId = ZoneIdCalculator.fromCoordinate(lastLat, lastLng)
                    if (zoneId != -1) {
                        // 채널에 쌓인 개별 좌표는 비워버리고 (이번 배치는 존 리포트로 대체)
                        while (locationChannel.tryReceive().isSuccess) { /* drain */ }

                        sendBuffer.append("zone=").append(zoneId)
                            .append(",density=").append(bleCount)
                            .append("\n")

                        println("[LocationRepository] 밀집 지역 감지(ble=$bleCount). 존 리포트 전송: zone=$zoneId")
                    }
                } else {
                    // ── 방법 1: 일반/희박 지역 → 격자 중복 제거 후 개별 좌표 배치 ──
                    dedupCells.clear()
                    while (true) {
                        val loc = locationChannel.tryReceive().getOrNull() ?: break
                        // 같은 100m 격자 안의 좌표는 최신 fix 한 건으로 압축
                        dedupCells[gridKey(loc.latitude, loc.longitude)] = loc
                    }
                    for (loc in dedupCells.values) {
                        appendLocationLine(loc.latitude, loc.longitude, bleCount)
                    }

                    // 정지 사용자 heartbeat: 새 fix가 없어도 추적 중이면
                    // 마지막 위치 한 줄을 보내 서버 5분 윈도우에서 살아 있게 유지
                    if (dedupCells.isEmpty() && tracking && hasLocation) {
                        appendLocationLine(lastLat, lastLng, bleCount)
                    }
                }

                if (sendBuffer.isNotEmpty()) {
                    val rawData = sendBuffer.toString()
                    val success = serverClient.sendBatchRaw(rawData)

                    if (!success) {
                        if (retryBuffer.length + rawData.length < MAX_RETRY_BUFFER_SIZE) {
                            retryBuffer.append(rawData)
                            println("[LocationRepository] 전송 실패. 재시도 버퍼에 보존합니다. (현재: ${retryBuffer.length} bytes)")
                        } else {
                            println("[LocationRepository] 재시도 버퍼 용량 초과. 일부 데이터를 폐기합니다.")
                        }
                    }
                }
            }
            isRunning.set(false)
        }
    }

    // 소수 5자리(≈1m)로 반올림해 직렬화: 서버 격자(약 100m) 대비 충분한
    // 정밀도이면서 줄 길이를 30% 이상 줄인다. (Locale.US: 소수점 '.' 보장)
    private fun appendLocationLine(lat: Double, lng: Double, bleCount: Int) {
        sendBuffer.append(userId)
            .append(',')
            .append(String.format(Locale.US, "%.5f", lat))
            .append(',')
            .append(String.format(Locale.US, "%.5f", lng))
        if (bleCount > 0) {
            sendBuffer.append(",ble=").append(bleCount)
        }
        sendBuffer.append('\n')
    }

    // 서버 SpatialHash::generateKey와 동일한 방식의 격자 키
    private fun gridKey(lat: Double, lng: Double): Long {
        val latIdx = floor(lat / DEDUP_GRID).toLong()
        val lngIdx = floor(lng / DEDUP_GRID).toLong()
        return (latIdx shl 32) or (lngIdx and 0xFFFFFFFFL)
    }
}
