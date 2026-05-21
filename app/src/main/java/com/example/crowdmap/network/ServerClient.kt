package com.example.crowdmap.network

import com.example.crowdmap.BuildConfig
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
        //val SERVER_IP: String = BuildConfig.SERVER_IP
        val SERVER_IP : String = "172.20.127.162"
        const val SERVER_PORT = 8765
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

    suspend fun getCongestion(
        latitude: Double,
        longitude: Double
    ): CongestionData? = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                val message = "0,$latitude,$longitude\n"
                writer?.write(message)
                writer?.flush()

                val response = reader?.readLine()
                println("[ServerClient] 조회 응답: $response")

                response?.let { parseResponse(it) }
            } catch (e: Exception) {
                println("[ServerClient] 조회 실패: ${e.message}")
                closeConnection()
                null
            }
        }
    }

    suspend fun sendLocation(
        userId: Int,
        latitude: Double,
        longitude: Double
    ): CongestionData? = withContext(Dispatchers.IO) {
        socketLock.withLock {
            try {
                val message = "$userId,$latitude,$longitude\n"
                writer?.write(message)
                writer?.flush()
                println("[ServerClient] 전송: $message")

                val response = reader?.readLine()
                println("[ServerClient] 응답: $response")

                response?.let { parseResponse(it) }
            } catch (e: Exception) {
                println("[ServerClient] 전송 실패: ${e.message}")
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
