package com.example.crowdmap.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
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
 * - 60초 슬라이딩 윈도우: 고유 MAC 주소 기준 중복 제거
 */
class BleScanner(private val context: Context) {

    companion object {
        // -70dBm 이상 = 약 10m 이내 기기만 밀도 추정에 사용
        private const val RSSI_THRESHOLD = -70
        // 60초 이내 감지된 기기만 유효 (배치 주기 10초의 6배)
        private const val WINDOW_MS = 60_000L
    }

    private val bluetoothAdapter: BluetoothAdapter? by lazy {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        manager?.adapter
    }

    // MAC 주소 → 마지막 감지 시각 (ms)
    private val detectedDevices = ConcurrentHashMap<String, Long>()
    private val isScanning = AtomicBoolean(false)

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (result.rssi >= RSSI_THRESHOLD) {
                detectedDevices[result.device.address] = System.currentTimeMillis()
            }
        }

        override fun onBatchScanResults(results: List<ScanResult>) {
            val now = System.currentTimeMillis()
            for (result in results) {
                if (result.rssi >= RSSI_THRESHOLD) {
                    detectedDevices[result.device.address] = now
                }
            }
        }
    }

    /**
     * BLE 스캔을 시작합니다. 이미 실행 중이면 무시합니다.
     * BLUETOOTH_SCAN 권한이 필요합니다 (Android 12+).
     */
    @SuppressLint("MissingPermission")
    fun startScan() {
        val scanner = bluetoothAdapter?.bluetoothLeScanner ?: return
        if (isScanning.getAndSet(true)) return

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        try {
            scanner.startScan(null, settings, scanCallback)
            println("[BleScanner] BLE 스캔 시작")
        } catch (e: Exception) {
            isScanning.set(false)
            println("[BleScanner] 스캔 시작 실패: ${e.message}")
        }
    }

    /**
     * BLE 스캔을 중지합니다.
     */
    @SuppressLint("MissingPermission")
    fun stopScan() {
        if (!isScanning.getAndSet(false)) return
        try {
            bluetoothAdapter?.bluetoothLeScanner?.stopScan(scanCallback)
            println("[BleScanner] BLE 스캔 중지")
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
     * BLE 기능 사용 가능 여부를 반환합니다.
     */
    fun isAvailable(): Boolean =
        bluetoothAdapter != null && bluetoothAdapter!!.isEnabled
}
