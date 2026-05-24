package com.example.crowdmap.location

import android.location.Location
import com.example.crowdmap.network.ServerClient
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import java.util.concurrent.atomic.AtomicBoolean

/**
 * [LocationRepository]
 * 고밀도 위치 데이터의 쓰로틀링, 배치 처리, 재시도 로직을 담당하는 핵심 도메인 계층.
 * GC 오버헤드와 메모리 파편화를 방지하기 위해 객체 재사용 및 비차단(Non-blocking) 설계를 적용함.
 */
class LocationRepository(
    private val serverClient: ServerClient,
    private val scope: CoroutineScope,
    private val userId: Int = 1001
) {
    // 1. 배압 제어: 버퍼가 가득 찰 경우 가장 오래된 데이터를 DROP하여 위치 수집 루프의 블로킹 방지
    private val locationChannel = Channel<Location>(
        capacity = 200,
        onBufferOverflow = BufferOverflow.DROP_OLDEST
    )

    // 2. 고정 크기 메모리 버퍼 재사용: 매 전송마다 새로운 StringBuilder를 생성하지 않음
    private val sendBuffer = StringBuilder(4096)
    private val retryBuffer = StringBuilder(8192) // 전송 실패 시 임시 보관용
    
    // 최대 보존 한계 (약 100개 지점 분량)
    private val MAX_RETRY_BUFFER_SIZE = 1024 * 10 

    private val isRunning = AtomicBoolean(false)

    init {
        startBatchWorker()
    }

    /**
     * 위치 데이터를 비차단 방식으로 채널에 적재
     */
    fun onLocationReceived(location: Location) {
        // trySend를 사용하여 Suspend 없이 즉시 결과 반환 (Non-blocking)
        locationChannel.trySend(location)
    }

    /**
     * 10초 주기로 버퍼링된 데이터를 병합하여 전송하는 백그라운드 워커
     */
    private fun startBatchWorker() {
        if (isRunning.getAndSet(true)) return

        scope.launch(Dispatchers.IO) {
            while (isActive) {
                delay(10_000L) // 10초 배치 주기

                // 전송용 임시 버퍼 초기화 (객체 재사용)
                sendBuffer.clear()

                // 1. 재시도 버퍼에 남은 데이터가 있다면 먼저 추가
                if (retryBuffer.isNotEmpty()) {
                    sendBuffer.append(retryBuffer)
                    retryBuffer.clear()
                }

                // 2. 현재 채널에 쌓인 모든 위치 데이터 추출 및 문자열 병합
                var count = 0
                while (true) {
                    val loc = locationChannel.tryReceive().getOrNull() ?: break
                    sendBuffer.append(userId)
                        .append(",")
                        .append(loc.latitude)
                        .append(",")
                        .append(loc.longitude)
                        .append("\n")
                    count++
                }

                if (sendBuffer.isNotEmpty()) {
                    val rawData = sendBuffer.toString()
                    val success = serverClient.sendBatchRaw(rawData)

                    if (!success) {
                        // 3. 전송 실패 시 재시도 버퍼에 다시 저장 (최대 용량 제한)
                        if (retryBuffer.length + rawData.length < MAX_RETRY_BUFFER_SIZE) {
                            retryBuffer.append(rawData)
                            println("[LocationRepository] 전송 실패. 데이터를 재시도 버퍼에 보존합니다. (현재: ${retryBuffer.length} bytes)")
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
