package com.example.crowdmap.network

import com.example.crowdmap.location.ZoneIdCalculator
import com.example.crowdmap.model.CongestionData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.BufferedWriter
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.ConcurrentHashMap

class ServerClient {

    companion object {
        //val SERVER_IP: String = BuildConfig.SERVER_IP
        val SERVER_IP : String = "172.20.127.154"

        const val SERVER_PORT = 8765
        const val CONNECT_TIMEOUT_MS = 5000

        // 조회 응답 대기 상한. socketLock이 왕복 시간만큼 잡히므로
        // (그동안 배치 전송이 대기) 너무 길면 폭증 시 지연이 연쇄된다.
        // 서버는 캐시/stale로 수십 ms 안에 응답하는 것이 정상 경로.
        const val READ_TIMEOUT_MS = 3000

        // ── 자동 재연결 백오프 ──
        // 서버 재시작 시 수천 대의 단말이 같은 순간 재접속을 시도하면
        // (reconnect storm) accept 큐가 터진다. 지수 백오프 + 무작위 지터로
        // 재접속 시도를 시간축에 분산시킨다.
        private const val RECONNECT_BASE_MS = 1_000L
        private const val RECONNECT_MAX_MS = 30_000L

        // ── 클라이언트 측 조회 캐시 TTL ──
        // 서버 CacheManager TTL(30초)의 절반. 같은 1km 존을 짧은 간격으로
        // 다시 조회하면 네트워크 왕복 없이 즉시 응답해 서버 조회 QPS를 줄인다.
        private const val QUERY_CACHE_TTL_MS = 15_000L
    }

    private var socket: Socket? = null
    private var writer: BufferedWriter? = null
    private var reader: BufferedReader? = null
    private val socketLock = Mutex()

    // 재연결 백오프 상태 (socketLock 안에서만 접근)
    private var reconnectDelayMs = RECONNECT_BASE_MS
    private var nextReconnectAtMs = 0L

    // zoneId → (혼잡도 결과, 수신 시각 ms)
    private val queryCache = ConcurrentHashMap<Int, Pair<CongestionData, Long>>()

    // 서버가 조회 응답에 실어준 권장 리포트 주기(초).
    // 자기 위치 조회(applyIntervalHint=true)에서만 갱신된다 —
    // 지도 탭(임의 지점) 조회의 힌트로 내 전송 주기를 바꾸면 안 되기 때문.
    // null이면 힌트 없음(기본 주기 사용).
    @Volatile
    var suggestedIntervalSec: Int? = null
        private set

    suspend fun connect(): Boolean = withContext(Dispatchers.IO) {
        socketLock.withLock { openConnection() }
    }

    // socketLock을 보유한 상태에서만 호출
    private fun openConnection(): Boolean {
        closeConnection()
        return try {
            val s = Socket()
            s.tcpNoDelay = true   // 소량 메시지를 모으지 않고 즉시 전송 (조회 지연 단축)
            s.keepAlive = true    // 죽은 연결을 커널 차원에서 감지
            s.connect(InetSocketAddress(SERVER_IP, SERVER_PORT), CONNECT_TIMEOUT_MS)
            s.soTimeout = READ_TIMEOUT_MS
            socket = s
            writer = BufferedWriter(OutputStreamWriter(s.getOutputStream(), "UTF-8"))
            reader = BufferedReader(InputStreamReader(s.getInputStream(), "UTF-8"))
            reconnectDelayMs = RECONNECT_BASE_MS
            nextReconnectAtMs = 0L
            println("[ServerClient] 서버 연결 성공")
            true
        } catch (e: Exception) {
            println("[ServerClient] 연결 실패: ${e.message}")
            false
        }
    }

    // 연결이 끊겨 있으면 백오프 일정에 따라 재연결을 시도한다.
    // 백오프 대기 중이면 시도 없이 false를 반환해 10초 배치 틱마다
    // 죽은 서버를 두드리지 않는다. (socketLock 보유 상태에서만 호출)
    private fun ensureConnected(): Boolean {
        if (isConnected()) return true

        val now = System.currentTimeMillis()
        if (now < nextReconnectAtMs) return false

        if (openConnection()) return true

        // full jitter: 다음 시도 시각을 [0.5×base, 현재 백오프] 범위에서 무작위 선택
        nextReconnectAtMs = now + RECONNECT_BASE_MS / 2 +
                (Math.random() * reconnectDelayMs).toLong()
        reconnectDelayMs = (reconnectDelayMs * 2).coerceAtMost(RECONNECT_MAX_MS)
        return false
    }

    /**
     * [Update] 고성능 배치 전송: 위치 데이터 갱신 시에는 서버로부터 응답을 기대하지 않음 (Silent Update)
     * 연결이 끊겨 있으면 백오프 일정에 따라 자동 재연결을 시도한다.
     */
    suspend fun sendBatchRaw(data: String): Boolean = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                if (!ensureConnected()) return@withLock false

                writer?.write(data)
                writer?.flush()
                // 서버는 Update에 대해 응답을 보내지 않으므로 read() 생략
                true
            } catch (e: Exception) {
                println("[ServerClient] 배치 전송 실패: ${e.message}")
                closeConnection()
                false
            }
        }
    }

    /**
     * [Query] 실시간 혼잡도 조회: 서버에 질의하고 응답을 수신함 (Request-Response)
     * 같은 존의 최근(15초 이내) 결과는 캐시에서 즉시 반환해 서버 부하와
     * 소켓 락 점유(왕복 대기 중 배치 전송 차단)를 함께 줄인다.
     *
     * @param applyIntervalHint 자기 위치 조회일 때만 true로 — 응답의
     *        "interval=N" 힌트를 위치 리포트 주기에 반영한다.
     */
    suspend fun getCongestion(
        latitude: Double,
        longitude: Double,
        applyIntervalHint: Boolean = false
    ): CongestionData? {
        val zoneId = ZoneIdCalculator.fromCoordinate(latitude, longitude)
        if (zoneId != -1) {
            queryCache[zoneId]?.let { (cached, fetchedAt) ->
                if (System.currentTimeMillis() - fetchedAt < QUERY_CACHE_TTL_MS) {
                    return cached
                }
            }
        }

        return withContext(Dispatchers.IO) {
            socketLock.withLock {
                try {
                    if (!ensureConnected()) return@withLock null

                    // userId=0은 조회 전용 프로토콜
                    val message = "0,$latitude,$longitude\n"
                    writer?.write(message)
                    writer?.flush()

                    // 서버는 userId=0인 경우에만 혼잡도 결과를 응답함
                    val response = reader?.readLine()
                    println("[ServerClient] 조회 응답 수신: $response")

                    val parsed = response?.let { parseResponse(it, zoneId) }
                    if (parsed != null) {
                        if (zoneId != -1) {
                            queryCache[zoneId] = parsed.first to System.currentTimeMillis()
                        }
                        if (applyIntervalHint) {
                            suggestedIntervalSec = parsed.second
                        }
                    }
                    parsed?.first
                } catch (e: Exception) {
                    println("[ServerClient] 조회 실패: ${e.message}")
                    closeConnection()
                    null
                }
            }
        }
    }

    // 응답: "LEVEL|ratio" + 선택 확장 필드("|interval=N" 등).
    // 알 수 없는 필드는 무시한다 — 서버가 필드를 추가해도 클라이언트가
    // 깨지지 않는 전방 호환(forward-compatible) 파싱.
    private fun parseResponse(response: String, zoneId: Int): Pair<CongestionData, Int?>? {
        val parts = response.trim().split("|")
        if (parts.size < 2) return null

        val data = CongestionData(
            level = parts[0].trim(),
            ratio = parts[1].trim().toDoubleOrNull() ?: 0.0,
            zoneId = zoneId
        )

        var intervalSec: Int? = null
        for (i in 2 until parts.size) {
            val field = parts[i].trim()
            if (field.startsWith("interval=")) {
                intervalSec = field.substringAfter("interval=").toIntOrNull()
            }
        }
        return data to intervalSec
    }

    fun disconnect() {
        closeConnection()
    }

    fun isConnected(): Boolean {
        val s = socket ?: return false
        return s.isConnected && !s.isClosed
    }

    private fun closeConnection() {
        try { writer?.close() } catch (_: Exception) {}
        try { reader?.close() } catch (_: Exception) {}
        try { socket?.close() } catch (_: Exception) {}
        writer = null
        reader = null
        socket = null
    }
}
