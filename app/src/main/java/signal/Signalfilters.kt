package com.example.crowdmap.signal // TODO: 실제 패키지명으로 변경

import android.bluetooth.le.ScanResult
import java.security.MessageDigest
import kotlin.math.abs
import kotlin.math.floor

/**
 * [우선순위 1] BLE 신호 품질 필터.
 *  - RSSI 컷오프: 중앙값 RSSI < -85dBm 단말 제외 → 100m 격자 밖 단말 카운트 오염 차단
 *  - 중앙값 필터: 순간 멀티패스 스파이크가 컷오프 판정을 흔들지 않게 함
 *  - MAC 회전 dedup: iOS/Android의 BLE MAC 랜덤화(~15분 주기) 직후,
 *    (광고 페이로드 지문 + RSSI 근접 + 시간 근접) 일치 시 동일 단말로 연결해 이중 카운트 방지
 */
class BleSignalFilter(
    private val rssiCutoffDbm: Int = -85,
    private val windowMs: Long = 60_000L,        // 기존 슬라이딩 윈도우와 동일
    private val linkWindowMs: Long = 8_000L,     // 회전 연결 판정: 옛 MAC 침묵 후 이 시간 내 등장
    private val linkRssiDeltaDbm: Int = 6
) {
    private class Track {
        val rssiSamples = ArrayDeque<Pair<Long, Int>>()  // (timestamp, rssi)
        var fingerprint: String = NO_FP
        var lastSeen = 0L
        var lastRssi = 0
        var aliasOf: String? = null                      // 회전 전 MAC (카운트 제외 표식)
    }

    private val tracks = HashMap<String, Track>()

    @Synchronized
    fun onScanResult(result: ScanResult, now: Long = System.currentTimeMillis()) {
        val mac = result.device.address
        val isNew = mac !in tracks
        val t = tracks.getOrPut(mac) { Track() }
        t.rssiSamples.addLast(now to result.rssi)
        while (t.rssiSamples.isNotEmpty() && now - t.rssiSamples.first().first > windowMs) {
            t.rssiSamples.removeFirst()
        }
        t.lastSeen = now
        t.lastRssi = result.rssi
        if (t.fingerprint == NO_FP) t.fingerprint = fingerprintOf(result)
        if (isNew) tryLinkRotatedMac(mac, t, now)
    }

    /** 윈도우 내 고유 단말 수: 회전 연결분(aliasOf != null) 제외, 중앙값 RSSI 컷오프 적용 */
    @Synchronized
    fun count(now: Long = System.currentTimeMillis()): Int {
        tracks.entries.removeAll { now - it.value.lastSeen > windowMs }
        return tracks.values.count { it.aliasOf == null && medianRssi(it) >= rssiCutoffDbm }
    }

    @Synchronized
    fun reset() = tracks.clear()

    private fun tryLinkRotatedMac(newMac: String, newT: Track, now: Long) {
        val fp = newT.fingerprint
        if (fp == NO_FP) return  // 페이로드 변별력 없는 단말은 오연결 위험 → 연결 안 함
        for ((mac, t) in tracks) {
            if (mac == newMac || t.aliasOf != null) continue
            val silentFor = now - t.lastSeen
            if (silentFor in 1..linkWindowMs &&
                t.fingerprint == fp &&
                abs(t.lastRssi - newT.lastRssi) <= linkRssiDeltaDbm
            ) {
                newT.aliasOf = mac
                return
            }
        }
    }

    private fun medianRssi(t: Track): Int {
        if (t.rssiSamples.isEmpty()) return Int.MIN_VALUE
        val sorted = t.rssiSamples.map { it.second }.sorted()
        return sorted[sorted.size / 2]
    }

    /** 광고 페이로드 지문 (raw bytes에 MAC은 포함되지 않으므로 회전과 무관하게 유지됨) */
    private fun fingerprintOf(result: ScanResult): String {
        val bytes = result.scanRecord?.bytes ?: return NO_FP
        if (bytes.count { it != 0.toByte() } < 4) return NO_FP  // 사실상 빈 페이로드
        return MessageDigest.getInstance("MD5").digest(bytes)
            .joinToString("") { "%02x".format(it) }
    }

    private companion object { const val NO_FP = "" }
}

/**
 * [우선순위 1] 좌표 EMA 필터.
 *  - 정지 시 GPS 떨림(±10m)을 평활화 → dedup 격자 키가 흔들리는 것 방지
 *  - 점프가 resetDistanceDeg(~50m) 초과 시 실제 이동으로 보고 즉시 추종(지연 없음)
 */
class EmaCoordinateFilter(
    private val alpha: Double = 0.3,
    private val resetDistanceDeg: Double = 0.0005
) {
    private var lat = Double.NaN
    private var lon = Double.NaN

    @Synchronized
    fun filter(rawLat: Double, rawLon: Double): Pair<Double, Double> {
        if (lat.isNaN() ||
            abs(rawLat - lat) > resetDistanceDeg ||
            abs(rawLon - lon) > resetDistanceDeg
        ) {
            lat = rawLat; lon = rawLon
        } else {
            lat = (1 - alpha) * lat + alpha * rawLat
            lon = (1 - alpha) * lon + alpha * rawLon
        }
        return lat to lon
    }

    @Synchronized
    fun reset() { lat = Double.NaN; lon = Double.NaN }
}

/**
 * [우선순위 1] 격자 경계 히스테리시스.
 *  - 경계에서 노이즈로 두 격자를 핑퐁하면 서버에서 양쪽 존 밀도가 동시에 부풀음
 *  - 새 격자에 연속 confirmCount회 들어와야 전환 확정. 미확정 동안은 직전 확정 좌표 유지
 *  - cellSizeDeg는 서버 SpatialHash(0.001°)와 동일해야 함
 */
class GridHysteresis(
    private val cellSizeDeg: Double = 0.001,
    private val confirmCount: Int = 2
) {
    private var currentCell: Pair<Long, Long>? = null
    private var candidateCell: Pair<Long, Long>? = null
    private var candidateHits = 0
    private var heldLat = 0.0
    private var heldLon = 0.0

    @Synchronized
    fun stabilize(lat: Double, lon: Double): Pair<Double, Double> {
        val cell = cellOf(lat, lon)
        when {
            currentCell == null || cell == currentCell -> {
                currentCell = cell
                candidateCell = null; candidateHits = 0
                heldLat = lat; heldLon = lon
                return lat to lon                         // 격자 안: 원본 좌표 그대로
            }
            cell == candidateCell -> {
                candidateHits++
                if (candidateHits >= confirmCount) {      // 전환 확정
                    currentCell = cell
                    candidateCell = null; candidateHits = 0
                    heldLat = lat; heldLon = lon
                    return lat to lon
                }
            }
            else -> { candidateCell = cell; candidateHits = 1 }
        }
        return heldLat to heldLon                          // 미확정: 직전 확정 좌표 유지
    }

    @Synchronized
    fun reset() { currentCell = null; candidateCell = null; candidateHits = 0 }

    private fun cellOf(lat: Double, lon: Double) =
        floor(lat / cellSizeDeg).toLong() to floor(lon / cellSizeDeg).toLong()
}