package com.example.crowdmap.signal // TODO: 실제 패키지명으로 변경

import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.bluetooth.le.ScanFilter
import android.content.Context
import android.os.ParcelUuid
import android.util.Log

/**
 * [우선순위 3] 앱 사용자 식별용 BLE 광고.
 *
 * 효과:
 *  1) 모든 CrowdMap 단말이 동일 Service UUID를 광고 → 스캐너가 ScanFilter로 '앱 사용자 수'를
 *     '전체 BLE 단말 수'와 분리 측정. 표본비율 보정(전체밀도 ≈ BLE총수 × 보정계수)의 기초 데이터.
 *  2) ScanFilter 매칭은 BT 컨트롤러(펌웨어)에서 수행 → 비매칭 패킷은 AP를 깨우지 않아
 *     필터 스캔은 상시 가동해도 전력 비용이 거의 없음.
 *
 * 필요 권한: BLUETOOTH_ADVERTISE (API 31+, 런타임)
 */
class CrowdAdvertiser(context: Context) {

    private val advertiser: BluetoothLeAdvertiser? =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)
            ?.adapter?.bluetoothLeAdvertiser            // null = 광고 미지원 단말(저가형 일부)

    @Volatile private var running = false

    private val callback = object : AdvertiseCallback() {
        override fun onStartFailure(errorCode: Int) {
            running = false
            Log.w(TAG, "advertise start failed: $errorCode")
        }
    }

    fun start() {
        if (running || advertiser == null) return
        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_POWER)   // ~1Hz, 전력 최소
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)   // ~100m 격자 정합
            .setConnectable(false)
            .build()
        val data = AdvertiseData.Builder()
            .addServiceUuid(SERVICE_UUID)
            .setIncludeDeviceName(false)                 // 페이로드 최소 + 프라이버시
            .setIncludeTxPowerLevel(false)
            .build()
        try {
            advertiser.startAdvertising(settings, data, callback)
            running = true
        } catch (_: SecurityException) {
            Log.w(TAG, "BLUETOOTH_ADVERTISE permission missing")
        }
    }

    fun stop() {
        if (!running) return
        try { advertiser?.stopAdvertising(callback) } catch (_: SecurityException) { }
        running = false
    }

    companion object {
        private const val TAG = "CrowdAdvertiser"

        /** CrowdMap 식별용 커스텀 128-bit UUID (전 단말 공통) */
        val SERVICE_UUID: ParcelUuid =
            ParcelUuid.fromString("c0dec0de-c0d3-4c0d-9c0d-c0dec0dec0de")

        /** 앱 사용자 카운트용 펌웨어 오프로드 필터 (상시 스캔에 사용) */
        fun appUserScanFilter(): List<ScanFilter> = listOf(
            ScanFilter.Builder().setServiceUuid(SERVICE_UUID).build()
        )
    }
}