package com.example.crowdmap.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import java.security.SecureRandom
import java.util.concurrent.atomic.AtomicBoolean

/**
 * [CrowdAdvertiser]
 * Apple/Google Exposure Notification(코로나 접촉 추적)과 같은 원리의
 * "회전 임시 ID(Rotating Ephemeral ID)" BLE 광고.
 *
 * 문제: Android/iOS는 프라이버시를 위해 BLE MAC 주소를 수 분~15분마다
 * 랜덤 변경한다. MAC 기준 dedup은 같은 단말을 회전 때마다 새 기기로
 * 세어 밀도를 부풀린다.
 *
 * 해법(EN 방식): 단말이 광고 페이로드에 직접 4바이트 임시 ID를 싣는다.
 *  - MAC이 바뀌어도 ID로 결정론적 dedup → 앱 사용자 카운트가 정확해진다.
 *  - ID 자체도 15분마다 회전하며 키와 무관한 순수 난수라 비가역 —
 *    세션 간 추적이 불가능하다(EN과 동일한 프라이버시 성질).
 *
 * 광고는 비연결성(non-connectable) + LOW_POWER 모드라 전력 부담이 작다.
 */
class CrowdAdvertiser(private val context: Context) {

    companion object {
        // CrowdMap 광고 식별용 서비스 UUID — BleScanner와 공유한다
        val SERVICE_UUID: ParcelUuid =
            ParcelUuid.fromString("8e4f7a52-3c2d-4e91-b1a5-0d6f3b2c9e71")

        // EN과 동일한 15분 회전 주기 (MAC 랜덤화 주기와 같은 차수)
        private const val ROTATION_MS = 15 * 60_000L

        const val ID_LENGTH = 4
    }

    private val bluetoothAdapter: BluetoothAdapter? by lazy {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        manager?.adapter
    }

    private val handler = Handler(Looper.getMainLooper())
    private val random = SecureRandom()
    private val isAdvertising = AtomicBoolean(false)

    @Volatile
    private var ephemeralId: ByteArray = newId()

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartFailure(errorCode: Int) {
            println("[CrowdAdvertiser] 광고 시작 실패: code=$errorCode")
        }
    }

    private val rotateRunnable = object : Runnable {
        override fun run() {
            if (!isAdvertising.get()) return
            rotateId()
            handler.postDelayed(this, ROTATION_MS)
        }
    }

    /** 임시 ID 광고를 시작합니다. BLUETOOTH_ADVERTISE 권한 필요 (Android 12+). */
    fun start() {
        if (bluetoothAdapter?.bluetoothLeAdvertiser == null) return
        if (isAdvertising.getAndSet(true)) return
        rotateId()   // 세션마다 새 ID로 시작 — 이전 세션과 연결 불가
        handler.postDelayed(rotateRunnable, ROTATION_MS)
        println("[CrowdAdvertiser] 임시 ID 광고 시작 (15분 회전)")
    }

    fun stop() {
        if (!isAdvertising.getAndSet(false)) return
        handler.removeCallbacks(rotateRunnable)
        stopAdvertising()
        println("[CrowdAdvertiser] 광고 중지")
    }

    /** 현재 광고 중인 내 임시 ID (소문자 hex 8자). 광고 꺼져 있으면 null. */
    fun currentIdHex(): String? =
        if (isAdvertising.get()) ephemeralId.toHex() else null

    private fun rotateId() {
        ephemeralId = newId()
        restartAdvertising()
    }

    private fun newId(): ByteArray = ByteArray(ID_LENGTH).also { random.nextBytes(it) }

    @SuppressLint("MissingPermission")
    private fun restartAdvertising() {
        stopAdvertising()
        val advertiser = bluetoothAdapter?.bluetoothLeAdvertiser ?: return

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_POWER)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_MEDIUM)
            .setConnectable(false)
            .build()

        // 31바이트 legacy 광고 한도: flags(3) + service data(128bit UUID 16 + 헤더 2
        // + 페이로드) ≤ 31 — 이름/TX파워는 제외해 공간을 확보한다.
        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(false)
            .setIncludeTxPowerLevel(false)
            .addServiceData(SERVICE_UUID, buildPayload())
            .build()

        try {
            advertiser.startAdvertising(settings, data, advertiseCallback)
        } catch (e: Exception) {
            println("[CrowdAdvertiser] 광고 재시작 실패: ${e.message}")
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopAdvertising() {
        try {
            bluetoothAdapter?.bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback)
        } catch (_: Exception) {
        }
    }

    /** 광고 페이로드: [4B 임시 ID] */
    private fun buildPayload(): ByteArray = ephemeralId.copyOf()

    fun isAvailable(): Boolean =
        bluetoothAdapter?.isEnabled == true && bluetoothAdapter?.bluetoothLeAdvertiser != null

    private fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }
}
