package com.example.crowdmap.network

import com.example.crowdmap.model.CongestionData
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.PrintWriter
import java.net.Socket

class ServerClient {

    companion object {
        //const val SERVER_IP = "172.20.127.144"  // Wi-Fi IP
        const val SERVER_IP = "10.0.2.2"  // 에뮬레이터용
        const val SERVER_PORT = 5001
    }

    private var socket: Socket? = null
    private var writer: PrintWriter? = null
    private var reader: BufferedReader? = null

    // 서버 연결
    suspend fun connect(): Boolean = withContext(Dispatchers.IO) {
        try {
            socket = Socket(SERVER_IP, SERVER_PORT)
            writer = PrintWriter(socket!!.getOutputStream(), true)
            reader = BufferedReader(InputStreamReader(socket!!.getInputStream()))
            println("[ServerClient] 서버 연결 성공")
            true
        } catch (e: Exception) {
            println("[ServerClient] 연결 실패: ${e.message}")
            false
        }
    }

    // 위치 전송 후 혼잡도 응답 받기
    suspend fun sendLocation(
        userId: Int,
        latitude: Double,
        longitude: Double
    ): CongestionData? = withContext(Dispatchers.IO) {
        try {
            val message = "$userId,$latitude,$longitude\n"
            writer?.print(message)
            writer?.flush()
            println("[ServerClient] 전송: $message")

            // 응답 읽기
            val buffer = CharArray(256)
            val length = reader?.read(buffer) ?: -1
            val response = if (length > 0) String(buffer, 0, length).trim() else null
            println("[ServerClient] 응답: $response")

            response?.let {
                val parts = it.split("|")
                if (parts.size == 2) {
                    CongestionData(
                        level = parts[0].trim(),
                        ratio = parts[1].trim().toDoubleOrNull() ?: 0.0,
                        zoneId = 0
                    )
                } else null
            }
        } catch (e: Exception) {
            println("[ServerClient] 전송 실패: ${e.message}")
            null
        }
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