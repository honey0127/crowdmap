package com.example.crowdmap.network

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

class ServerClient {

    companion object {
        val SERVER_IP : String = "192.168.0.45"
        const val SERVER_PORT = 5001
        const val CONNECT_TIMEOUT_MS = 5000
        const val READ_TIMEOUT_MS = 5000
    }

    private var socket: Socket? = null
    private var writer: BufferedWriter? = null
    private var reader: BufferedReader? = null
    private val socketLock = Mutex()

    suspend fun connect(): Boolean = withContext(Dispatchers.IO) {
        try {
            val s = Socket()
            s.connect(InetSocketAddress(SERVER_IP, SERVER_PORT), CONNECT_TIMEOUT_MS)
            s.soTimeout = READ_TIMEOUT_MS
            socket = s
            writer = BufferedWriter(OutputStreamWriter(s.getOutputStream(), "UTF-8"))
            reader = BufferedReader(InputStreamReader(s.getInputStream(), "UTF-8"))
            println("[ServerClient] 서버 연결 성공")
            true
        } catch (e: Exception) {
            println("[ServerClient] 연결 실패: ${e.message}")
            false
        }
    }

    /**
     * [Update] 고성능 배치 전송: 위치 데이터 갱신 시에는 서버로부터 응답을 기대하지 않음 (Silent Update)
     */
    suspend fun sendBatchRaw(data: String): Boolean = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                if (!isConnected()) return@withLock false
                
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
     * [Query] 실시간 혼잡도 조회: 맵 클릭 시 서버에 질의하고 응답을 수신함 (Request-Response)
     */
    suspend fun getCongestion(
        latitude: Double,
        longitude: Double
    ): CongestionData? = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                if (!isConnected()) return@withLock null

                // userId=0은 조회 전용 프로토콜
                val message = "0,$latitude,$longitude\n"
                writer?.write(message)
                writer?.flush()

                // 서버는 userId=0인 경우에만 혼잡도 결과를 응답함
                val response = reader?.readLine()
                println("[ServerClient] 조회 응답 수신: $response")

                response?.let { parseResponse(it) }
            } catch (e: Exception) {
                println("[ServerClient] 조회 실패: ${e.message}")
                closeConnection()
                null
            }
        }
    }

    private fun parseResponse(response: String): CongestionData? {
        val parts = response.trim().split("|")
        return if (parts.size == 2) {
            CongestionData(
                level = parts[0].trim(),
                ratio = parts[1].trim().toDoubleOrNull() ?: 0.0,
                zoneId = 0
            )
        } else null
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
