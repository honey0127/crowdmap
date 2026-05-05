package com.example.crowdmap.network

import com.example.crowdmap.model.CongestionData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.InetSocketAddress
import java.net.Socket

class ServerClient {

    companion object {
        const val SERVER_IP = "34.22.82.9"
        const val SERVER_PORT = 5001
        const val CONNECT_TIMEOUT_MS = 5000
        const val READ_TIMEOUT_MS = 5000
    }

    private var socket: Socket? = null
    private var writer: PrintWriter? = null
    private var reader: BufferedReader? = null
    private val socketLock = Mutex()

    // 서버 연결
    suspend fun connect(): Boolean = withContext(Dispatchers.IO) {
        try {
            val s = Socket()
            s.connect(InetSocketAddress(SERVER_IP, SERVER_PORT), CONNECT_TIMEOUT_MS)
            s.soTimeout = READ_TIMEOUT_MS
            socket = s
            writer = PrintWriter(s.getOutputStream(), true)
            reader = BufferedReader(InputStreamReader(s.getInputStream()))
            println("[ServerClient] 서버 연결 성공")
            true
        } catch (e: Exception) {
            println("[ServerClient] 연결 실패: ${e.message}")
            false
        }
    }

    // 특정 위치의 혼잡도만 조회 (userId=0으로 구분)
    suspend fun getCongestion(
        latitude: Double,
        longitude: Double
    ): CongestionData? = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                val message = "0,$latitude,$longitude\n"
                writer?.print(message)
                writer?.flush()

                val response = reader?.readLine()
                println("[ServerClient] 조회 응답: $response")

                response?.let { parseResponse(it) }
            } catch (e: Exception) {
                println("[ServerClient] 조회 실패: ${e.message}")
                null
            }
        }
    }

    // 위치 전송 후 혼잡도 응답 받기
    suspend fun sendLocation(
        userId: Int,
        latitude: Double,
        longitude: Double
    ): CongestionData? = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                val message = "$userId,$latitude,$longitude\n"
                writer?.print(message)
                writer?.flush()
                println("[ServerClient] 전송: $message")

                val response = reader?.readLine()
                println("[ServerClient] 응답: $response")

                response?.let { parseResponse(it) }
            } catch (e: Exception) {
                println("[ServerClient] 전송 실패: ${e.message}")
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

    // 연결 종료
    fun disconnect() {
        try {
            writer?.close()
            reader?.close()
            socket?.close()
            println("[ServerClient] 연결 종료")
        } catch (e: Exception) {
            println("[ServerClient] 종료 오류: ${e.message}")
        }
    }

    // 연결 상태 확인
    fun isConnected(): Boolean {
        return socket?.isConnected == true && socket?.isClosed == false
    }
}