package com.example.crowdmap.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * BLE Passive Scanning을 통해 주변 BLE 기기 수를 추정합니다.
 *
 * 스마트폰, 이어폰, 스마트워치 등 모든 BLE 기기는 자신의 존재를 알리는
 * 광고 패킷을 주기적으로 브로드캐스트합니다. 페어링 없이도 이 패킷을 수신할 수 있어
 * 주변 인파 밀도를 추정하는 보정 데이터로 활용합니다.
 *
 * - RSSI >= -70dBm 필터: 약 10m 이내 기기만 카운트
 * - 60초 슬라이딩 윈도우 중복 제거:
 *   · CrowdMap 앱 단말 → 광고 페이로드의 회전 임시 ID 기준 (EN 방식, 결정론적)
 *   · 그 외 BLE 기기 → MAC 주소 기준 (OS의 MAC 랜덤화 때문에 근사치)
 *   MAC 랜덤화로 같은 단말이 회전 때마다 새 기기로 집계되던 문제가,
 *   앱 사용자에 한해서는 광고 ID로 정확히 제거된다.
 *
 * ── 수집 효율화 ──
 * 1. 듀티 사이클 스캔: 30초 주기 중 8초만 라디오를 켠다. 60초 슬라이딩
 *    윈도우가 스캔 공백을 메워주므로 고유 기기 카운트는 거의 그대로
 *    유지되면서 라디오 점유 시간을 ~73% 줄인다. 밀도 추정은 10초 배치
 *    주기로만 소비되므로 상시 LOW_LATENCY 스캔은 순수 낭비였다.
 *    (Android 7+의 "30초당 스캔 시작 5회 제한"에도 안전: 30초당 1회 시작)
 * 2. 하드웨어 오프로드 배칭: 지원 단말에서는 광고 패킷마다 앱을 깨우지
 *    않고 BT 컨트롤러 펌웨어가 2초간 모아 onBatchScanResults로 한 번에
 *    전달한다. 군중 밀집 지역(초당 수백 패킷)에서 콜백 횟수가 급감한다.
 */
class BleScanner(private val context: Context) {

    companion object {
        // -70dBm 이상 = 약 10m 이내 기기만 밀도 추정에 사용
        private const val RSSI_THRESHOLD = -70
        // 60초 이내 감지된 기기만 유효 (배치 주기 10초의 6배)
        private const val WINDOW_MS = 60_000L
        // 듀티 사이클: SCAN_CYCLE_MS 주기 중 SCAN_ACTIVE_MS 동안만 라디오 ON
        private const val SCAN_ACTIVE_MS = 8_000L
        private const val SCAN_CYCLE_MS = 30_000L
        // 컨트롤러가 스캔 결과를 모아서 전달하는 간격 (오프로드 배칭 지원 시)
        private const val REPORT_DELAY_MS = 2_000L
    }

    private val bluetoothAdapter: BluetoothAdapter? by lazy {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        manager?.adapter
    }

    // dedup 키("id:<hex>" 또는 "mac:<addr>") → 마지막 감지 시각 (ms)
    private val detectedDevices = ConcurrentHashMap<String, Long>()
    // CrowdMap 앱 단말의 광고 임시 ID(hex) → 마지막 감지 시각 (클러스터 헤드 선출용)
    private val appDeviceIds = ConcurrentHashMap<String, Long>()
    // 이웃 조난 단말의 위탁 관측치: zoneId → (density, 감지 시각)
    // 같은 존은 최신 관측으로 덮어쓴다 (서버도 존 리포트를 덮어쓰기로 집계)
    private val relayObservations = ConcurrentHashMap<Int, Pair<Int, Long>>()
    private val isScanRequested = AtomicBoolean(false)  // 외부에서 요청한 스캔 상태
    private val isRadioOn = AtomicBoolean(false)        // 실제 라디오 동작 여부

    private val handler = Handler(Looper.getMainLooper())

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            record(result, System.currentTimeMillis())
        }

        override fun onBatchScanResults(results: List<ScanResult>) {
            val now = System.currentTimeMillis()
            for (result in results) {
                record(result, now)
            }
        }
    }

    private fun record(result: ScanResult, now: Long) {
        if (result.rssi < RSSI_THRESHOLD) return

        // CrowdMap 단말이면 광고 페이로드의 회전 임시 ID로 dedup —
        // MAC이 랜덤 변경돼도 같은 단말은 같은 키로 집계된다 (EN 방식)
        val serviceData = result.scanRecord?.serviceData?.get(CrowdAdvertiser.SERVICE_UUID)
        if (serviceData != null && serviceData.size >= CrowdAdvertiser.ID_LENGTH) {
            val idHex = StringBuilder(CrowdAdvertiser.ID_LENGTH * 2).apply {
                for (i in 0 until CrowdAdvertiser.ID_LENGTH) {
                    append("%02x".format(serviceData[i]))
                }
            }.toString()
            detectedDevices["id:$idHex"] = now
            appDeviceIds[idHex] = now

            // 조난 페이로드(9B): 업로드가 막힌 이웃의 존 관측치 — 위탁 수신
            if (serviceData.size >= CrowdAdvertiser.DISTRESS_LENGTH) {
                val base = CrowdAdvertiser.ID_LENGTH
                val zoneId = ((serviceData[base].toInt() and 0xFF) shl 24) or
                             ((serviceData[base + 1].toInt() and 0xFF) shl 16) or
                             ((serviceData[base + 2].toInt() and 0xFF) shl 8) or
                             (serviceData[base + 3].toInt() and 0xFF)
                val density = serviceData[base + 4].toInt() and 0xFF
                if (zoneId >= 0 && density > 0) {
                    relayObservations[zoneId] = density to now
                }
            }
        } else {
            detectedDevices["mac:${result.device.address}"] = now
        }
    }

    /**
     * 이웃 조난 단말로부터 위탁받은 존 관측치를 회수(consume)합니다.
     * 회수된 항목은 비워져 같은 관측이 매 배치 중복 송신되지 않는다.
     * (조난 단말이 계속 광고 중이면 다음 스캔에서 다시 채워진다 —
     *  서버 존 리포트 TTL 5분 내 주기적 갱신 효과)
     */
    fun takeRelayObservations(maxAgeMs: Long): List<Pair<Int, Int>> {
        val cutoff = System.currentTimeMillis() - maxAgeMs
        val out = ArrayList<Pair<Int, Int>>()
        val it = relayObservations.entries.iterator()
        while (it.hasNext()) {
            val e = it.next()
            if (e.value.second >= cutoff) out.add(e.key to e.value.first)
            it.remove()
        }
        return out
    }

    // ON ↔ OFF 상태를 오가며 자기 자신을 다시 스케줄링하는 듀티 사이클 루프
    private val cycleRunnable = object : Runnable {
        override fun run() {
            if (!isScanRequested.get()) return
            if (isRadioOn.get()) {
                stopRadio()
                handler.postDelayed(this, SCAN_CYCLE_MS - SCAN_ACTIVE_MS)
            } else {
                startRadio()
                handler.postDelayed(this, SCAN_ACTIVE_MS)
            }
        }
    }

    /**
     * BLE 듀티 사이클 스캔을 시작합니다. 이미 실행 중이면 무시합니다.
     * BLUETOOTH_SCAN 권한이 필요합니다 (Android 12+).
     */
    fun startScan() {
        if (bluetoothAdapter?.bluetoothLeScanner == null) return
        if (isScanRequested.getAndSet(true)) return
        handler.post(cycleRunnable)
        println("[BleScanner] BLE 듀티 사이클 스캔 시작 (${SCAN_ACTIVE_MS / 1000}s ON / ${SCAN_CYCLE_MS / 1000}s 주기)")
    }

    /**
     * BLE 스캔을 중지합니다.
     */
    fun stopScan() {
        if (!isScanRequested.getAndSet(false)) return
        handler.removeCallbacks(cycleRunnable)
        stopRadio()
        println("[BleScanner] BLE 스캔 중지")
    }

    @SuppressLint("MissingPermission")
    private fun startRadio() {
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
        if (isRadioOn.getAndSet(true)) return

        // 듀티 사이클 + 60초 윈도우 집계 구조에서는 BALANCED로 충분하다.
        // LOW_LATENCY는 라디오를 거의 100% 점유해 발열/배터리 부담이 크다.
        val builder = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_BALANCED)
        if (bluetoothAdapter?.isOffloadedScanBatchingSupported == true) {
            builder.setReportDelay(REPORT_DELAY_MS)
        }

        try {
            scanner.startScan(null, builder.build(), scanCallback)
        } catch (e: Exception) {
            isRadioOn.set(false)
            println("[BleScanner] 스캔 시작 실패: ${e.message}")
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopRadio() {
        if (!isRadioOn.getAndSet(false)) return
        try {
            val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
            // 오프로드 배칭 사용 시 컨트롤러에 남아 있는 결과를 회수하고 종료
            scanner.flushPendingScanResults(scanCallback)
            scanner.stopScan(scanCallback)
        } catch (e: Exception) {
            println("[BleScanner] 스캔 중지 실패: ${e.message}")
        }
    }

    /**
     * 최근 WINDOW_MS 안에 감지된 고유 BLE 기기 수를 반환합니다.
     * 오래된 항목은 자동으로 제거됩니다.
     */
    fun getCount(): Int {
        val cutoff = System.currentTimeMillis() - WINDOW_MS
        detectedDevices.entries.removeIf { it.value < cutoff }
        return detectedDevices.size
    }

    /**
     * 최근 windowMs 안에 감지된 CrowdMap 앱 단말의 광고 ID(hex) 목록.
     * 클러스터 헤드 선출(사전순 최소 ID)에서 사용합니다.
     */
    fun getActiveAppIds(windowMs: Long): List<String> {
        val cutoff = System.currentTimeMillis() - windowMs
        appDeviceIds.entries.removeIf { it.value < cutoff }
        return appDeviceIds.keys.toList()
    }

    /**
     * BLE 기능 사용 가능 여부를 반환합니다.
     */
    fun isAvailable(): Boolean =
        bluetoothAdapter != null && bluetoothAdapter!!.isEnabled
}
