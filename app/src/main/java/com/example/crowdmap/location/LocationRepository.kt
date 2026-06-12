package com.example.crowdmap.location

import android.location.Location
import com.example.crowdmap.ble.BleScanner
import com.example.crowdmap.ble.CrowdAdvertiser
import com.example.crowdmap.model.CongestionData
import com.example.crowdmap.network.ServerClient
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.roundToInt
import kotlin.math.sign
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
    private val bleScanner: BleScanner? = null,
    private val advertiser: CrowdAdvertiser? = null
) {
    companion object {
        // 이 수 이상 BLE 기기가 감지되면 밀집 지역으로 판단 → 존 리포트 전송
        private const val DENSE_ZONE_THRESHOLD = 5

        private const val BATCH_INTERVAL_MS = 10_000L
        private const val BATCH_JITTER_MS = 1_000L

        // 밀집 지역(존 리포트 모드)의 최소 전송 주기. 개별 좌표 대신
        // 존 리포트 한 줄이 집계를 대표하므로(서버 TTL 5분) 주기를 늦춰도
        // 정확도 손실 없이 서버 유입량만 줄어든다.
        private const val DENSE_INTERVAL_MS = 30_000L

        // 클러스터 헤드 선출 윈도우: 밀집 주기(30초)의 3배.
        // 현 헤드의 광고가 3주기 동안 안 들리면 ID가 윈도우에서 빠지고
        // 다음으로 작은 ID 단말이 자동 승계한다 (Zigbee/Thread 라우터
        // 선출과 같은 무합의(coordination-free) 방식).
        private const val HEAD_ELECTION_WINDOW_MS = 90_000L

        // ── Find My식 기회적 릴레이 ──
        // 이 횟수 이상 연속 전송 실패 시 내 관측치를 광고에 실어 이웃에 위탁
        private const val DISTRESS_AFTER_FAILURES = 2
        // 위탁 관측치의 유효 나이 / 배치당 릴레이 줄 수 상한
        private const val RELAY_MAX_AGE_MS = 60_000L
        private const val RELAY_MAX_LINES = 5

        // ── 장기 체류(Visit) 감지 감압 ──
        // Google Sleep API / Apple Visit Monitoring과 같은 발상: "이 장소에
        // 머무르는 중"이 확실해지면 신호 빈도를 더 낮춰도 정보 손실이 없다.
        // 같은 100m 격자에 30분 이상 머문 단말은 heartbeat 주기를 60초로
        // 강하한다. 서버 밀도는 5분 윈도우 집계라 60초 heartbeat로도
        // 사용자가 계속 카운트되며, 서버 유휴 정리(90초)보다 짧아 연결도
        // 유지된다.
        private const val DWELL_THRESHOLD_MS = 30 * 60_000L
        private const val DWELL_INTERVAL_MS = 60_000L

        // ── 로컬 차등 프라이버시(LDP) ──
        // Apple이 이모지/QuickType 통계에 실사용하는 방식: 단말이 송신 전에
        // 노이즈를 더해 개별 값으로는 단말 주변 환경을 특정할 수 없게 하고,
        // 서버는 다수 표본 집계에서 노이즈가 상쇄된 값을 복원한다.
        // ε=1.0 → Laplace(b=1/ε), 표준편차 약 ±1.4 기기.
        private const val LDP_EPSILON = 1.0

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

    // 장기 체류 감지: 현재 머무는 100m 격자와 그 격자에 들어온 시각.
    // 격자가 바뀌면 리셋된다. (정지 사용자는 이동 필터 때문에 fix가 안
    // 와서 격자가 안 바뀌므로 체류 시간이 자연히 누적된다)
    @Volatile private var dwellCellKey: Long = Long.MIN_VALUE
    @Volatile private var dwellStartMs: Long = 0L

    // ── RRC(셀룰러 라디오) 번들링 결과 콜백 ──
    // 셀룰러 라디오는 송신 후 ~10초 고전력 tail을 유지하므로, 작은 패킷을
    // 띄엄띄엄 보내면 tail만 누적된다(Android 공식 'bundled transfers' 가이드).
    // 자기 위치 혼잡도 조회를 위치 fix마다 따로 하지 않고, 배치 전송 직후
    // 라디오가 이미 깨어 있는 같은 burst에 합쳐 수행하고 결과를 이 콜백으로
    // 전달한다. 송신량은 같고 라디오 웨이크업 횟수만 줄어든다.
    @Volatile var onBatchResult: ((CongestionData?) -> Unit)? = null

    // 연속 전송 실패 수 (배치 워커 코루틴 전용)
    private var consecutiveSendFailures = 0

    private val isRunning = AtomicBoolean(false)

    init {
        startBatchWorker()
    }

    fun setTracking(active: Boolean) {
        tracking = active
        // 추적을 (재)시작하면 체류 시간을 새로 센다 — 이전 세션의 체류가
        // 이월되어 시작부터 60초 주기로 떨어지는 것을 방지
        if (active) {
            dwellStartMs = System.currentTimeMillis()
        }
    }

    fun onLocationReceived(location: Location) {
        lastLat     = location.latitude
        lastLng     = location.longitude
        hasLocation = true

        // 격자가 바뀌면 체류 타이머 리셋, 같은 격자 안 이동은 체류 지속
        val cell = gridKey(location.latitude, location.longitude)
        if (cell != dwellCellKey) {
            dwellCellKey = cell
            dwellStartMs = System.currentTimeMillis()
        }

        locationChannel.trySend(location)
    }

    private fun startBatchWorker() {
        if (isRunning.getAndSet(true)) return

        scope.launch(Dispatchers.IO) {
            // 다음 배치까지의 대기 시간. 서버 힌트(interval=N)와 밀집 모드에
            // 따라 매 주기 다시 계산된다.
            var nextDelayMs = BATCH_INTERVAL_MS

            while (isActive) {
                delay(nextDelayMs + Random.nextLong(-BATCH_JITTER_MS, BATCH_JITTER_MS))

                sendBuffer.clear()

                if (retryBuffer.isNotEmpty()) {
                    sendBuffer.append(retryBuffer)
                    retryBuffer.clear()
                }

                val bleCount = bleScanner?.getCount() ?: 0
                // 서버로 나가는 모든 BLE 관측값은 LDP 노이즈를 거친다.
                // 밀집 판정(DENSE_ZONE_THRESHOLD)은 전송되지 않는 로컬 판단이라
                // 원본 값을 사용 — 노이즈로 모드가 떨리는 것을 방지.
                val noisyBle = if (bleCount > 0) ldpNoise(bleCount) else 0
                var denseMode = false

                // ── 방법 2: 밀집 지역 → 존 밀도 리포트로 대체 ──────────────────
                // 주변 기기가 DENSE_ZONE_THRESHOLD 이상이면 개별 좌표 대신
                // 경량 "zone=<id>,density=<N>" 한 줄만 전송해 서버 부하를 낮춘다.
                if (bleCount >= DENSE_ZONE_THRESHOLD && hasLocation) {
                    val zoneId = ZoneIdCalculator.fromCoordinate(lastLat, lastLng)
                    if (zoneId != -1) {
                        denseMode = true
                        // 채널에 쌓인 개별 좌표는 비워버리고 (이번 배치는 존 리포트로 대체)
                        while (locationChannel.tryReceive().isSuccess) { /* drain */ }

                        // ── 클러스터 헤드 선출 ──────────────────────────────
                        // 같은 존의 단말들이 각자 리포트하면 N대가 N줄을 보낸다.
                        // 센서 메시 표준대로 "광고 ID 사전순 최소" 단말 1대만
                        // 송신하고 나머지는 침묵 → 존당 업스트림이 정확히 1로
                        // 수렴한다. ID는 고정 길이 hex라 사전순 == 숫자 비교.
                        // 내 광고가 꺼져 있으면(권한 거부 등) 선출 불참 → 송신.
                        val myId = advertiser?.currentIdHex()
                        val heardIds = bleScanner?.getActiveAppIds(HEAD_ELECTION_WINDOW_MS)
                            ?: emptyList()
                        val isClusterHead = myId == null || heardIds.none { it < myId }

                        if (isClusterHead) {
                            // density=0이면 서버가 버리므로 최소 1 보장
                            sendBuffer.append("zone=").append(zoneId)
                                .append(",density=").append(noisyBle.coerceAtLeast(1))
                                .append("\n")
                            println("[LocationRepository] 밀집 지역 감지(ble=$bleCount). 클러스터 헤드로 존 리포트 전송: zone=$zoneId")
                        } else {
                            println("[LocationRepository] 밀집 지역 — 더 작은 ID의 헤드 존재. 존 리포트 침묵 (들리는 앱 단말 ${heardIds.size}대)")
                        }
                    }
                }

                if (!denseMode) {
                    // ── 방법 1: 일반/희박 지역 → 격자 중복 제거 후 개별 좌표 배치 ──
                    dedupCells.clear()
                    while (true) {
                        val loc = locationChannel.tryReceive().getOrNull() ?: break
                        // 같은 100m 격자 안의 좌표는 최신 fix 한 건으로 압축
                        dedupCells[gridKey(loc.latitude, loc.longitude)] = loc
                    }
                    for (loc in dedupCells.values) {
                        appendLocationLine(loc.latitude, loc.longitude, noisyBle)
                    }

                    // 정지 사용자 heartbeat: 새 fix가 없어도 추적 중이면
                    // 마지막 위치 한 줄을 보내 서버 5분 윈도우에서 살아 있게 유지
                    if (dedupCells.isEmpty() && tracking && hasLocation) {
                        appendLocationLine(lastLat, lastLng, noisyBle)
                    }
                }

                // ── Find My식 릴레이: 이웃 조난 단말의 관측치를 내 배치에 합승 ──
                // 망 포화로 직접 업로드가 막힌 단말의 존 리포트를, 연결이 성한
                // 내가 대신 올린다. 직접 들은 광고만 릴레이하므로(1-hop) 루프 없음.
                val relays = bleScanner?.takeRelayObservations(RELAY_MAX_AGE_MS) ?: emptyList()
                var relayed = 0
                for ((zone, density) in relays) {
                    if (relayed >= RELAY_MAX_LINES) break
                    sendBuffer.append("zone=").append(zone)
                        .append(",density=").append(density)
                        .append("\n")
                    relayed++
                }
                if (relayed > 0) {
                    println("[LocationRepository] 이웃 조난 관측 ${relayed}건 릴레이 합승")
                }

                var sentOk = true
                if (sendBuffer.isNotEmpty()) {
                    val rawData = sendBuffer.toString()
                    sentOk = serverClient.sendBatchRaw(rawData)

                    if (!sentOk) {
                        if (retryBuffer.length + rawData.length < MAX_RETRY_BUFFER_SIZE) {
                            retryBuffer.append(rawData)
                            println("[LocationRepository] 전송 실패. 재시도 버퍼에 보존합니다. (현재: ${retryBuffer.length} bytes)")
                        } else {
                            println("[LocationRepository] 재시도 버퍼 용량 초과. 일부 데이터를 폐기합니다.")
                        }
                    }
                }

                // ── 조난 광고 전환 (store-and-forward 위탁) ──
                // 연속 실패가 쌓이면(셀룰러 RAN 포화 등) 내 존 관측치를 BLE
                // 광고에 실어 이웃이 대신 업로드하게 한다. 망이 포화될수록
                // 주변에 연결 성한 단말이 있을 확률은 높아지므로, 포화
                // 지역일수록 데이터가 더 잘 모이는 역설적 강건성이 생긴다.
                if (!sentOk) {
                    consecutiveSendFailures++
                    if (consecutiveSendFailures >= DISTRESS_AFTER_FAILURES &&
                        tracking && hasLocation
                    ) {
                        val myZone = ZoneIdCalculator.fromCoordinate(lastLat, lastLng)
                        if (myZone != -1) {
                            // 위탁 관측치도 원점에서 1회만 노이즈 적용 (릴레이는 그대로 전달)
                            advertiser?.setDistress(myZone, maxOf(noisyBle, 1))
                        }
                    }
                } else {
                    consecutiveSendFailures = 0
                    advertiser?.clearDistress()
                }

                // ── RRC 번들링: 배치 송신으로 라디오가 깨어 있는 지금,
                //    같은 burst에 자기 위치 혼잡도 조회를 합친다 ──
                // 전송이 실패했으면(연결 문제) 조회도 건너뛴다 — 백오프가 처리.
                // 이 조회가 서버의 interval 힌트도 함께 실어온다.
                if (tracking && hasLocation && sentOk) {
                    val result = serverClient.getCongestion(
                        lastLat, lastLng, applyIntervalHint = true
                    )
                    onBatchResult?.invoke(result)
                }

                // ── 다음 주기 결정 (서버 주도 적응형 감압) ──
                // 서버가 자기 위치 조회 응답에 실어준 권장 주기를 따른다.
                // (혼잡 존일수록 김 → 서버가 유입량을 원격으로 조절)
                // 밀집 모드에서는 최소 30초: 존 리포트가 집계를 대표하므로
                // 자주 보낼 이유가 없다.
                val hintMs = (serverClient.suggestedIntervalSec?.coerceIn(10, 60) ?: 10) * 1000L
                var delayMs = if (denseMode) maxOf(hintMs, DENSE_INTERVAL_MS) else hintMs

                // 장기 체류 감압: 같은 격자에 30분 이상 머무르면 60초로 강하.
                // (서버 유휴 정리 90초 미만 유지 — 연결은 살아 있다)
                if (tracking && hasLocation && dwellStartMs > 0 &&
                    System.currentTimeMillis() - dwellStartMs >= DWELL_THRESHOLD_MS
                ) {
                    delayMs = maxOf(delayMs, DWELL_INTERVAL_MS)
                }
                nextDelayMs = delayMs
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

    // Laplace(0, 1/ε) 노이즈를 더한 값 (음수는 0 클램프).
    // 개별 송신값은 ±수 기기 수준으로 흐려져 "이 단말 주변에 정확히
    // 몇 대가 있었나"를 서버가 복원할 수 없고, 존 단위 집계는 표본이
    // 모이며 노이즈가 상쇄된다. 수집 단계부터 프라이버시가 보장되는
    // 구조(privacy at collection).
    // 참고: 현 서버 BLE 보정은 max 기반이라 평균 기반 대비 +1~2의
    // 상향 편향이 생기지만 혼잡 레벨 구간 폭 대비 무시 가능한 수준.
    private fun ldpNoise(value: Int): Int {
        val u = Random.nextDouble() - 0.5
        val safe = (1.0 - 2.0 * abs(u)).coerceAtLeast(1e-12)  // ln(0) 방지
        val noise = -(1.0 / LDP_EPSILON) * sign(u) * ln(safe)
        return (value + noise).roundToInt().coerceAtLeast(0)
    }

    // 서버 SpatialHash::generateKey와 동일한 방식의 격자 키
    private fun gridKey(lat: Double, lng: Double): Long {
        val latIdx = floor(lat / DEDUP_GRID).toLong()
        val lngIdx = floor(lng / DEDUP_GRID).toLong()
        return (latIdx shl 32) or (lngIdx and 0xFFFFFFFFL)
    }
}
