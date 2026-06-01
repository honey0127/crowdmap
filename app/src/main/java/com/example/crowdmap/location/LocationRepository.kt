package com.example.crowdmap.location

import android.location.Location
import com.example.crowdmap.ble.BleScanner
import com.example.crowdmap.network.ServerClient
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import java.util.concurrent.atomic.AtomicBoolean

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
    }

    private val locationChannel = Channel<Location>(
        capacity = 200,
        onBufferOverflow = BufferOverflow.DROP_OLDEST
    )

    private val sendBuffer    = StringBuilder(4096)
    private val retryBuffer   = StringBuilder(8192)
    private val MAX_RETRY_BUFFER_SIZE = 1024 * 10

    // 최신 위치 (존 리포트에 사용)
    @Volatile private var lastLat: Double = 0.0
    @Volatile private var lastLng: Double = 0.0
    @Volatile private var hasLocation: Boolean = false

    private val isRunning = AtomicBoolean(false)

    init {
        startBatchWorker()
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
                delay(10_000L)

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
                    // ── 방법 1: 일반/희박 지역 → BLE count 포함 개별 좌표 배치 ──
                    var count = 0
                    while (true) {
                        val loc = locationChannel.tryReceive().getOrNull() ?: break
                        sendBuffer.append(userId)
                            .append(",")
                            .append(loc.latitude)
                            .append(",")
                            .append(loc.longitude)
                        if (bleCount > 0) {
                            sendBuffer.append(",ble=").append(bleCount)
                        }
                        sendBuffer.append("\n")
                        count++
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
}
