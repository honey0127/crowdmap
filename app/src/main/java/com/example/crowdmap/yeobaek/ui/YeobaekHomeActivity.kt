package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.ScheduleRequest
import com.example.crowdmap.yeobaek.data.YeobaekClient
import com.google.android.gms.maps.CameraUpdateFactory
import com.google.android.gms.maps.GoogleMap
import com.google.android.gms.maps.OnMapReadyCallback
import com.google.android.gms.maps.SupportMapFragment
import com.google.android.gms.maps.model.BitmapDescriptorFactory
import com.google.android.gms.maps.model.LatLng
import com.google.android.gms.maps.model.LatLngBounds
import com.google.android.gms.maps.model.MapStyleOptions
import com.google.android.gms.maps.model.MarkerOptions
import com.google.android.material.button.MaterialButton
import com.google.android.material.button.MaterialButtonToggleGroup
import com.google.android.material.chip.Chip
import com.google.android.material.chip.ChipGroup
import com.google.android.material.datepicker.MaterialDatePicker
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.timepicker.MaterialTimePicker
import com.google.android.material.timepicker.TimeFormat
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/**
 * 홈(재설계, 네이버지도×테이블링): 풀스크린 지도 + 떠 있는 검색바 + 하단 시트.
 *  - 검색으로 담은 장소를 지도에 마커로 한눈에 보여준다.
 *  - "코스 방식" 토글: [자동 최적화]=혼잡도 예측으로 순서 재배치 / [내 순서대로]=고른 순서 유지.
 */
class YeobaekHomeActivity : AppCompatActivity(), OnMapReadyCallback {

    private data class Stop(val title: String, val lat: Double, val lng: Double) {
        val hasLatLng: Boolean get() = !lat.isNaN() && !lng.isNaN()
    }

    private val selectedStops = LinkedHashMap<Long, Stop>()  // id → 정보(순서 보존)
    private var date: LocalDate = LocalDate.now()
    private var time: LocalTime = LocalTime.of(14, 0)
    private var keepOrder = false

    private var map: GoogleMap? = null

    private lateinit var dateField: TextInputEditText
    private lateinit var timeField: TextInputEditText
    private lateinit var chipGroup: ChipGroup
    private lateinit var emptyHint: TextView
    private lateinit var progress: ProgressBar
    private lateinit var planButton: MaterialButton
    private lateinit var modeHint: TextView

    private lateinit var recoPanel: View
    private lateinit var recoAdapter: RecoAdapter
    private var recoJob: Job? = null

    private val searchLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            val id = result.data?.getLongExtra(Extras.PLACE_ID, -1L) ?: -1L
            val title = result.data?.getStringExtra(Extras.PLACE_TITLE) ?: return@registerForActivityResult
            val lat = result.data?.getDoubleExtra(Extras.PLACE_LAT, Double.NaN) ?: Double.NaN
            val lng = result.data?.getDoubleExtra(Extras.PLACE_LNG, Double.NaN) ?: Double.NaN
            if (id > 0) addStop(id, Stop(title, lat, lng))
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_home)

        dateField = findViewById(R.id.input_date)
        timeField = findViewById(R.id.input_time)
        chipGroup = findViewById(R.id.chip_group)
        emptyHint = findViewById(R.id.stops_empty)
        progress = findViewById(R.id.home_progress)
        planButton = findViewById(R.id.btn_plan)
        modeHint = findViewById(R.id.mode_hint)

        dateField.setOnClickListener { pickDate() }
        timeField.setOnClickListener { pickTime() }
        val openSearch = View.OnClickListener { searchLauncher.launch(Intent(this, SearchActivity::class.java)) }
        findViewById<View>(R.id.search_bar).setOnClickListener(openSearch)
        findViewById<MaterialButton>(R.id.btn_add_place).setOnClickListener(openSearch)
        planButton.setOnClickListener { requestSchedule() }

        // 지역 추천 패널
        recoPanel = findViewById(R.id.reco_panel)
        recoAdapter = RecoAdapter { place -> addFromReco(place) }
        findViewById<RecyclerView>(R.id.reco_list).apply {
            layoutManager = LinearLayoutManager(
                this@YeobaekHomeActivity, LinearLayoutManager.HORIZONTAL, false)
            adapter = recoAdapter
        }
        findViewById<View>(R.id.reco_close).setOnClickListener { recoPanel.visibility = View.GONE }

        val modeGroup = findViewById<MaterialButtonToggleGroup>(R.id.mode_group)
        modeGroup.check(R.id.btn_mode_auto)
        modeGroup.addOnButtonCheckedListener { _, checkedId, isChecked ->
            if (!isChecked) return@addOnButtonCheckedListener
            keepOrder = checkedId == R.id.btn_mode_keep
            modeHint.text = if (keepOrder)
                "내가 고른 순서 그대로 진행하고, 각 장소의 도착 시점 혼잡도만 알려줘요"
            else
                "혼잡도를 예측해 방문 순서를 자동으로 조정해요"
        }

        (supportFragmentManager.findFragmentById(R.id.map_fragment) as SupportMapFragment)
            .getMapAsync(this)

        updateDateText()
        updateTimeText()
        refreshChips()
    }

    override fun onMapReady(googleMap: GoogleMap) {
        map = googleMap
        runCatching {
            googleMap.setMapStyle(MapStyleOptions.loadRawResourceStyle(this, R.raw.ye_map_style))
        }
        googleMap.uiSettings.isMapToolbarEnabled = false
        // 기본 시점: 서울 중심
        googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(LatLng(37.5665, 126.9780), 13f))
        // 지도를 옮길 때마다 그 지역 명소를 추천
        googleMap.setOnCameraIdleListener { refreshReco() }
        renderMarkers()
        refreshReco()
    }

    /** 현재 지도 중심 주변 명소를 추천 패널에 채운다(디바운스). */
    private fun refreshReco() {
        val gmap = map ?: return
        val center = gmap.cameraPosition.target
        val radius = radiusKmFromMap(gmap)
        recoJob?.cancel()
        recoJob = lifecycleScope.launch {
            delay(450)   // 카메라가 멈춘 뒤에만 요청(스팸 방지)
            try {
                val res = YeobaekClient.api.nearbyPlaces(center.latitude, center.longitude, radius)
                val fresh = res.results.filter { !selectedStops.containsKey(it.contentId) }
                recoAdapter.submit(fresh)
                recoPanel.visibility = if (fresh.isEmpty()) View.GONE else View.VISIBLE
            } catch (e: Exception) {
                // 추천은 부가 기능 — 실패 시 조용히 숨김
                recoPanel.visibility = View.GONE
            }
        }
    }

    private fun addFromReco(place: PlaceResult) {
        addStop(place.contentId, Stop(place.title, place.lat ?: Double.NaN, place.lng ?: Double.NaN))
        Toast.makeText(this, "‘${place.title}’ 플래너에 담았어요", Toast.LENGTH_SHORT).show()
        refreshReco()   // 방금 담은 항목은 추천에서 제외
    }

    /** 보이는 지도 범위(중심→모서리)로 추천 반경(km)을 정한다. */
    private fun radiusKmFromMap(gmap: GoogleMap): Double {
        val c = gmap.cameraPosition.target
        val ne = gmap.projection.visibleRegion.latLngBounds.northeast
        val d = haversineKm(c.latitude, c.longitude, ne.latitude, ne.longitude)
        return d.coerceIn(0.6, 12.0)
    }

    private fun haversineKm(lat1: Double, lng1: Double, lat2: Double, lng2: Double): Double {
        val r = 6371.0088
        val dLat = Math.toRadians(lat2 - lat1)
        val dLng = Math.toRadians(lng2 - lng1)
        val a = sin(dLat / 2) * sin(dLat / 2) +
            cos(Math.toRadians(lat1)) * cos(Math.toRadians(lat2)) * sin(dLng / 2) * sin(dLng / 2)
        return 2 * r * asin(min(1.0, sqrt(a)))
    }

    private fun pickDate() {
        val picker = MaterialDatePicker.Builder.datePicker()
            .setTitleText("날짜 선택")
            .setSelection(date.atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli())
            .build()
        picker.addOnPositiveButtonClickListener { millis ->
            date = Instant.ofEpochMilli(millis).atZone(ZoneOffset.UTC).toLocalDate()
            updateDateText()
        }
        picker.show(supportFragmentManager, "date")
    }

    private fun pickTime() {
        val picker = MaterialTimePicker.Builder()
            .setTimeFormat(TimeFormat.CLOCK_24H)
            .setHour(time.hour).setMinute(time.minute)
            .setTitleText("출발 시각")
            .build()
        picker.addOnPositiveButtonClickListener {
            time = LocalTime.of(picker.hour, picker.minute)
            updateTimeText()
        }
        picker.show(supportFragmentManager, "time")
    }

    private fun updateDateText() =
        dateField.setText(date.format(DateTimeFormatter.ofPattern("M월 d일 (E)", Locale.KOREAN)))

    private fun updateTimeText() =
        timeField.setText(time.format(DateTimeFormatter.ofPattern("HH:mm")))

    private fun addStop(id: Long, stop: Stop) {
        if (selectedStops.containsKey(id)) {
            Toast.makeText(this, "이미 추가된 장소예요", Toast.LENGTH_SHORT).show(); return
        }
        selectedStops[id] = stop
        refreshChips()
        renderMarkers()
    }

    private fun refreshChips() {
        chipGroup.removeAllViews()
        emptyHint.visibility = if (selectedStops.isEmpty()) View.VISIBLE else View.GONE
        for ((id, stop) in selectedStops) {
            val chip = Chip(this).apply {
                text = stop.title
                isCloseIconVisible = true
                setOnCloseIconClickListener {
                    selectedStops.remove(id); refreshChips(); renderMarkers()
                }
            }
            chipGroup.addView(chip)
        }
    }

    /** 선택 장소를 그린 마커로 그리고 카메라를 담긴 좌표에 맞춘다. */
    private fun renderMarkers() {
        val gmap = map ?: return
        gmap.clear()
        val pts = ArrayList<LatLng>()
        for (stop in selectedStops.values) {
            if (!stop.hasLatLng) continue
            val pos = LatLng(stop.lat, stop.lng)
            pts.add(pos)
            gmap.addMarker(
                MarkerOptions().position(pos).title(stop.title)
                    .icon(BitmapDescriptorFactory.defaultMarker(153f))  // 브랜드 그린
            )
        }
        when (pts.size) {
            0 -> Unit
            1 -> gmap.animateCamera(CameraUpdateFactory.newLatLngZoom(pts[0], 14f))
            else -> {
                val b = LatLngBounds.Builder().apply { pts.forEach { include(it) } }.build()
                gmap.animateCamera(CameraUpdateFactory.newLatLngBounds(b, 160))
            }
        }
    }

    private fun requestSchedule() {
        if (selectedStops.isEmpty()) {
            Toast.makeText(this, "장소를 하나 이상 추가하세요", Toast.LENGTH_SHORT).show(); return
        }
        val startTime = LocalDateTime.of(date, time)
            .format(DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:00"))
        val stops = selectedStops.keys.toList()

        setLoading(true)
        lifecycleScope.launch {
            try {
                val plan = YeobaekClient.api.schedule(
                    ScheduleRequest(startTime = startTime, stops = stops, keepOrder = keepOrder)
                )
                val json = YeobaekClient.gson.toJson(plan)
                startActivity(
                    Intent(this@YeobaekHomeActivity, PlannerActivity::class.java).apply {
                        putExtra(Extras.STOPS, stops.toLongArray())
                        putExtra(Extras.START_TIME, startTime)
                        putExtra(Extras.PLAN_JSON, json)
                        putExtra(Extras.KEEP_ORDER, keepOrder)
                    }
                )
            } catch (e: Exception) {
                Toast.makeText(this@YeobaekHomeActivity,
                    "코스 생성 실패: ${e.message ?: "네트워크 오류"} (서버 IP/실행 확인)",
                    Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
            }
        }
    }

    private fun setLoading(loading: Boolean) {
        progress.visibility = if (loading) View.VISIBLE else View.GONE
        planButton.isEnabled = !loading
    }
}
