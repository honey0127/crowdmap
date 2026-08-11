package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.graphics.Rect
import android.os.Bundle
import android.view.View
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.AdhocRequest
import com.example.crowdmap.yeobaek.data.Congestion
import com.example.crowdmap.yeobaek.data.EcoStore
import com.example.crowdmap.yeobaek.data.StampStore
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.ReportRequest
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
import com.google.android.gms.maps.model.Marker
import com.google.android.gms.maps.model.MarkerOptions
import com.google.android.gms.maps.model.PointOfInterest
import com.google.android.material.bottomsheet.BottomSheetBehavior
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
import java.time.ZoneId
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
    private lateinit var sheetSummary: TextView
    private lateinit var sheetSummarySub: TextView
    private lateinit var sheetBehavior: BottomSheetBehavior<View>

    private lateinit var recoPanel: View
    private lateinit var recoAdapter: RecoAdapter
    private var recoJob: Job? = null

    private val selectedMarkers = HashMap<Long, Marker>()      // 담긴 장소(코럴 핀)
    private val nearbyMarkers = HashMap<Marker, PlaceResult>()  // 주변 추천(그린 핀)
    private lateinit var placeInfo: View
    private lateinit var infoTitle: TextView
    private lateinit var infoMeta: TextView
    private lateinit var infoAdd: MaterialButton
    private lateinit var infoOffpeak: MaterialButton
    private lateinit var infoDisperse: MaterialButton
    private lateinit var recoHeader: TextView
    private lateinit var ecoChip: TextView
    private lateinit var stampChip: TextView
    private lateinit var placeInfoBehavior: BottomSheetBehavior<View>
    private val reportMarkers = ArrayList<Marker>()

    private val searchLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            val id = result.data?.getLongExtra(Extras.PLACE_ID, -1L) ?: -1L
            val title = result.data?.getStringExtra(Extras.PLACE_TITLE) ?: return@registerForActivityResult
            val lat = result.data?.getDoubleExtra(Extras.PLACE_LAT, Double.NaN) ?: Double.NaN
            val lng = result.data?.getDoubleExtra(Extras.PLACE_LNG, Double.NaN) ?: Double.NaN
            if (id <= 0) return@registerForActivityResult
            // 검색 결과는 바로 담지 않고, 그 위치로 이동해 장소 시트를 띄운다(네이버지도식).
            // 사용자가 시트의 '＋ 플래너에 추가'를 눌러야 코스에 들어간다.
            if (!lat.isNaN() && !lng.isNaN()) {
                map?.animateCamera(CameraUpdateFactory.newLatLngZoom(LatLng(lat, lng), 16f))
            }
            showPlaceInfo(PlaceResult(
                contentId = id, title = title,
                lat = lat.takeIf { !it.isNaN() }, lng = lng.takeIf { !it.isNaN() }))
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
        sheetSummary = findViewById(R.id.sheet_summary)
        sheetSummarySub = findViewById(R.id.sheet_summary_sub)

        // 하단 시트: 드래그로 접고 펼침. 손잡이를 탭해도 토글된다.
        sheetBehavior = BottomSheetBehavior.from(findViewById<View>(R.id.bottom_sheet))
        sheetBehavior.state = BottomSheetBehavior.STATE_COLLAPSED
        findViewById<View>(R.id.sheet_handle_area).setOnClickListener {
            sheetBehavior.state =
                if (sheetBehavior.state == BottomSheetBehavior.STATE_EXPANDED)
                    BottomSheetBehavior.STATE_COLLAPSED
                else
                    BottomSheetBehavior.STATE_EXPANDED
        }

        dateField.setOnClickListener { pickDate() }
        timeField.setOnClickListener { pickTime() }
        val openSearch = View.OnClickListener { searchLauncher.launch(Intent(this, SearchActivity::class.java)) }
        findViewById<View>(R.id.search_bar).setOnClickListener(openSearch)
        findViewById<MaterialButton>(R.id.btn_add_place).setOnClickListener(openSearch)
        planButton.setOnClickListener { requestSchedule() }
        // 지금 보고 있는 지도 중심을 기준으로 코스 플래너를 연다.
        findViewById<MaterialButton>(R.id.btn_district).setOnClickListener { openAreaPlanner() }

        // 지역 추천 패널
        recoPanel = findViewById(R.id.reco_panel)
        recoAdapter = RecoAdapter { place -> addFromReco(place) }
        findViewById<RecyclerView>(R.id.reco_list).apply {
            layoutManager = LinearLayoutManager(
                this@YeobaekHomeActivity, LinearLayoutManager.HORIZONTAL, false)
            adapter = recoAdapter
        }
        findViewById<View>(R.id.reco_close).setOnClickListener { recoPanel.visibility = View.GONE }

        // 장소 정보 시트(네이버지도식) — 탭/검색했을 때만 아래에서 올라온다.
        placeInfo = findViewById(R.id.place_info)
        infoTitle = findViewById(R.id.info_title)
        infoMeta = findViewById(R.id.info_meta)
        infoAdd = findViewById(R.id.info_add)
        infoOffpeak = findViewById(R.id.info_offpeak)
        infoDisperse = findViewById(R.id.info_disperse)
        recoHeader = findViewById(R.id.reco_header)
        placeInfoBehavior = BottomSheetBehavior.from(placeInfo)
        placeInfoBehavior.state = BottomSheetBehavior.STATE_HIDDEN   // 기본은 숨김
        findViewById<View>(R.id.info_close).setOnClickListener { hidePlaceInfo() }

        // 에코 트래블러 배지 + 실시간 제보
        ecoChip = findViewById(R.id.eco_chip)
        updateEcoChip()
        findViewById<MaterialButton>(R.id.btn_report).setOnClickListener { showReportDialog() }

        // 여백 스탬프(모은 장소) — 누르면 모아보기
        stampChip = findViewById(R.id.stamp_chip)
        updateStampChip()
        stampChip.setOnClickListener { showStampsDialog() }

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
        // 접힌 하단 시트에 구글 로고·컨트롤이 가리지 않도록 여백을 준다(시트 peek 높이 기준).
        val bottomPad = (128 * resources.displayMetrics.density).toInt()
        googleMap.setPadding(0, 0, 0, bottomPad)
        // 기본 시점: 서울 중심
        googleMap.moveCamera(CameraUpdateFactory.newLatLngZoom(LatLng(37.5665, 126.9780), 13f))
        // 지도를 옮길 때마다 그 지역 명소를 추천
        googleMap.setOnCameraIdleListener { refreshReco() }
        // 핀 탭 → 정보 카드(네이버지도식). 지도의 명소 라벨 탭도 여백 DB에서 찾아 카드로.
        googleMap.setOnMarkerClickListener { marker ->
            val p = nearbyMarkers[marker]
            if (p != null) { showPlaceInfo(p); true } else false
        }
        googleMap.setOnPoiClickListener { poi -> handlePoiTap(poi) }
        googleMap.setOnMapClickListener { hidePlaceInfo() }
        // 아무 지점이나 길게 누르면 그 위치를 '선택한 위치'로 담을 수 있게(어드혹)
        googleMap.setOnMapLongClickListener { ll ->
            showPlaceInfo(PlaceResult(contentId = 0, title = "선택한 위치",
                lat = ll.latitude, lng = ll.longitude))
        }
        // 구성 변경 등으로 이미 담긴 장소가 있으면 마커 복원
        selectedStops.forEach { (id, stop) -> addSelectedMarker(id, stop) }
        fitCameraToSelected()
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
                // 히트맵: 주변 명소 + 현재 혼잡 레벨(핀 색) + 한적함 지수
                val res = YeobaekClient.api.heatmap(center.latitude, center.longitude, radius)
                val fresh = res.results.filter { !selectedStops.containsKey(it.contentId) }
                // 빈 지역에서 아무 표시도 없으면 고장처럼 보인다 — 왜 비었는지 알려준다.
                recoHeader.text = if (fresh.isEmpty())
                    "이 지역엔 등록된 명소가 없어요"
                else
                    "이 지역 추천 · 플래너에 넣을까요?"
                recoAdapter.submit(fresh)
                if (!isPlaceInfoShown()) recoPanel.visibility = View.VISIBLE
                renderNearbyMarkers(fresh)
                loadReports()
            } catch (e: Exception) {
                // 추천은 부가 기능 — 실패 시 조용히 숨김
                recoPanel.visibility = View.GONE
            }
        }
    }

    /**
     * 주변 추천 장소를 '점 + 이름' 라벨로 표시(탭하면 정보 시트).
     *
     * 핀만 잔뜩 찍히면 구분이 안 되므로 이름을 직접 그린다(네이버지도식).
     * 라벨끼리 겹치면 읽을 수 없으니, 화면 좌표로 겹침을 검사해 겹치는 것은 생략한다.
     * 한적한 곳(quiet_score 높은 순)을 먼저 배치해 추천 대상이 우선 살아남게 한다.
     * 지도를 확대하면 간격이 벌어져 가려졌던 이름이 다시 나타난다.
     */
    private fun renderNearbyMarkers(list: List<PlaceResult>) {
        val gmap = map ?: return
        nearbyMarkers.keys.forEach { it.remove() }
        nearbyMarkers.clear()

        val proj = gmap.projection
        val dm = resources.displayMetrics
        val placed = ArrayList<Rect>()
        val margin = (2 * dm.density).toInt()

        for (p in list.sortedByDescending { it.quietScore ?: 0 }) {
            val lat = p.lat ?: continue
            val lng = p.lng ?: continue
            val pos = LatLng(lat, lng)
            // 레벨이 없으면(예보권 밖·키 미설정) 중립 회색 — 모르는 걸 '보통'으로 속이지 않는다.
            val label = MapLabel.render(dm, p.title, Congestion.color(p.level ?: 0))

            // 화면상 차지할 영역을 구해 이미 놓인 라벨과 겹치는지 확인
            val pt = proj.toScreenLocation(pos)
            val left = pt.x - label.bitmap.width / 2
            val top = pt.y - (label.anchorY * label.bitmap.height).toInt()
            val rect = Rect(left - margin, top - margin,
                left + label.bitmap.width + margin, top + label.bitmap.height + margin)
            if (placed.any { Rect.intersects(it, rect) }) continue
            placed.add(rect)

            val m = gmap.addMarker(
                MarkerOptions().position(pos)
                    .icon(BitmapDescriptorFactory.fromBitmap(label.bitmap))
                    .anchor(label.anchorX, label.anchorY)
            ) ?: continue
            nearbyMarkers[m] = p
        }
    }

    private fun addFromReco(place: PlaceResult) {
        // DB 명소(content_id>0)는 바로 담고, 어드혹(0)은 서버에 즉석 등록 후 담는다.
        if (place.contentId > 0) {
            addStop(place.contentId,
                Stop(place.title, place.lat ?: Double.NaN, place.lng ?: Double.NaN))
            Toast.makeText(this, "‘${place.title}’ 플래너에 담았어요", Toast.LENGTH_SHORT).show()
            // 에코 리워드: 한적한 곳(여유/보통)을 담으면 포인트
            if ((place.level ?: 9) <= 2) awardEco(EcoStore.P_QUIET_ADD)
            return
        }
        val lat = place.lat; val lng = place.lng
        if (lat == null || lng == null) return
        lifecycleScope.launch {
            try {
                val res = YeobaekClient.api.addAdhoc(AdhocRequest(place.title, lat, lng))
                addStop(res.contentId, Stop(res.title, lat, lng))
                Toast.makeText(this@YeobaekHomeActivity,
                    "‘${res.title}’ 담았어요", Toast.LENGTH_SHORT).show()
            } catch (e: Exception) {
                Toast.makeText(this@YeobaekHomeActivity,
                    "담기 실패: ${e.message ?: "서버 확인"}", Toast.LENGTH_SHORT).show()
            }
        }
    }

    /** 지도 명소 라벨(구글 POI) 탭 →
     *  ① 라벨의 한글 이름으로 검색(구글 라벨은 영문+한글 혼합이라 한글만 추출),
     *  ② 실패하면 탭한 좌표에서 가장 가까운 여백 명소(반경 250m)로 매칭,
     *  ③ 둘 다 없으면 근처에 등록 명소가 없다고 안내(초록 핀 유도). */
    private fun handlePoiTap(poi: PointOfInterest) {
        val ll = poi.latLng
        val korean = poi.name.split("\n").map { it.trim() }
            .firstOrNull { seg -> seg.any { it in '가'..'힣' } } ?: poi.name.trim()
        lifecycleScope.launch {
            try {
                val byName = korean.takeIf { it.isNotBlank() }
                    ?.let { YeobaekClient.api.searchPlaces(it, 1).results.firstOrNull() }
                if (byName != null) { showPlaceInfo(byName); return@launch }
                val byLoc = YeobaekClient.api
                    .nearbyPlaces(ll.latitude, ll.longitude, 0.4, 1).results.firstOrNull()
                if (byLoc != null && (byLoc.distKm ?: 9.9) <= 0.25) {
                    showPlaceInfo(byLoc)
                } else {
                    // DB 에 없는 명소 — 이 위치 자체를 담을 수 있게 제시(어드혹).
                    showPlaceInfo(PlaceResult(contentId = 0,
                        title = korean.ifBlank { "선택한 위치" },
                        lat = ll.latitude, lng = ll.longitude))
                }
            } catch (e: Exception) { /* 부가 기능 — 조용히 무시 */ }
        }
    }

    private fun showPlaceInfo(place: PlaceResult) {
        infoTitle.text = place.title
        val dist = place.distKm?.let { "%.1fkm".format(it) }
        infoMeta.text = if (place.contentId <= 0)
            "지도에서 선택한 위치 · 담아서 코스에 추가"
        else {
            val congestion = place.level?.let { Congestion.label(it) }
            val quiet = place.quietScore?.let { "한적함 $it" }
            listOfNotNull(place.catLabel, congestion, quiet, dist)
                .joinToString(" · ")
                .ifBlank { "플래너에 추가해 코스를 만들어 보세요" }
        }
        val already = selectedStops.containsKey(place.contentId)
        infoAdd.text = if (already) "플래너에 담김" else "＋ 플래너에 추가"
        infoAdd.isEnabled = !already
        infoAdd.setOnClickListener { addFromReco(place); hidePlaceInfo() }
        // 오프피크/미시분산은 DB 명소(content_id>0)에서만
        val isDb = place.contentId > 0
        infoOffpeak.visibility = if (isDb) View.VISIBLE else View.GONE
        infoDisperse.visibility = if (isDb) View.VISIBLE else View.GONE
        infoOffpeak.setOnClickListener { showOffpeak(place) }
        infoDisperse.setOnClickListener { showDisperse(place) }
        // 지역 추천 패널과 코스 시트는 접어 두고, 장소 시트만 올린다(네이버지도식).
        recoPanel.visibility = View.GONE
        sheetBehavior.state = BottomSheetBehavior.STATE_COLLAPSED
        placeInfoBehavior.state = BottomSheetBehavior.STATE_EXPANDED
    }

    /** 오프피크(덜 붐비는 시간) — 향후 12h 중 저혼잡 시간대 top3 다이얼로그. */
    private fun showOffpeak(place: PlaceResult) {
        lifecycleScope.launch {
            try {
                val res = YeobaekClient.api.offpeak(place.contentId)
                if (res.best.isEmpty()) {
                    Toast.makeText(this@YeobaekHomeActivity,
                        "이 지역은 예보가 없어 오프피크 추천이 어려워요(서울권만)",
                        Toast.LENGTH_SHORT).show()
                    return@launch
                }
                val fmt = DateTimeFormatter.ofPattern("HH:mm")
                val lines = res.best.joinToString("\n") { h ->
                    val t = java.time.Instant.ofEpochSecond(h.unix)
                        .atZone(ZoneId.of("Asia/Seoul")).toLocalTime().format(fmt)
                    "· $t   ${Congestion.label(h.level)}  (한적함 ${h.quietScore ?: "-"})"
                }
                AlertDialog.Builder(this@YeobaekHomeActivity)
                    .setTitle("‘${place.title}’ 덜 붐비는 시간")
                    .setMessage(lines)
                    .setPositiveButton("좋아요", null)
                    .show()
                awardEco(EcoStore.P_OFFPEAK)
            } catch (e: Exception) {
                Toast.makeText(this@YeobaekHomeActivity, "오프피크 조회 실패",
                    Toast.LENGTH_SHORT).show()
            }
        }
    }

    /** 미시적 분산 — 도보권 더 한적한 대안을 추천 카루셀에 채운다. */
    private fun showDisperse(place: PlaceResult) {
        lifecycleScope.launch {
            try {
                val res = YeobaekClient.api.disperse(place.contentId)
                val fresh = res.results.filter { !selectedStops.containsKey(it.contentId) }
                if (fresh.isEmpty()) {
                    Toast.makeText(this@YeobaekHomeActivity,
                        "근처에 더 한적한 대안이 없어요", Toast.LENGTH_SHORT).show()
                    return@launch
                }
                recoHeader.text = "🚶 ‘${place.title}’ 근처 더 한적한 곳"
                recoAdapter.submit(fresh)
                renderNearbyMarkers(fresh)
                hidePlaceInfo()
                recoPanel.visibility = View.VISIBLE
            } catch (e: Exception) {
                Toast.makeText(this@YeobaekHomeActivity, "대안 조회 실패",
                    Toast.LENGTH_SHORT).show()
            }
        }
    }

    private fun updateEcoChip() {
        ecoChip.text = "🌱 ${EcoStore.points(this)}"
    }

    private fun updateStampChip() {
        stampChip.text = "旅 ${StampStore.count(this)}"
    }

    /** 지금까지 모은 스탬프(장소) 모아보기. */
    private fun showStampsDialog() {
        val stamps = StampStore.all(this)
        val body = if (stamps.isEmpty()) {
            "아직 모은 스탬프가 없어요.\n지도에서 장소를 담으면 스탬프가 쌓여요."
        } else {
            stamps.joinToString("\n") { "旅 ${it.title}" }
        }
        AlertDialog.Builder(this)
            .setTitle("여백 스탬프 (${stamps.size})")
            .setMessage(body)
            .setPositiveButton("닫기", null)
            .show()
    }

    private fun awardEco(pts: Int) {
        val total = EcoStore.award(this, pts)
        updateEcoChip()
        Toast.makeText(this, "🌱 +$pts 에코 포인트 · ${EcoStore.badge(total)}",
            Toast.LENGTH_SHORT).show()
    }

    /** 이 지점 실시간 제보(붐빔/한적/팁). */
    private fun showReportDialog() {
        val gmap = map ?: return
        val c = gmap.cameraPosition.target
        val labels = arrayOf("😖 지금 붐벼요", "😌 여기 한적해요", "💡 팁 남기기")
        val kinds = arrayOf("busy", "quiet", "tip")
        AlertDialog.Builder(this)
            .setTitle("이 지점(지도 중심) 실시간 제보")
            .setItems(labels) { _, which -> postReport(kinds[which], c.latitude, c.longitude) }
            .show()
    }

    private fun postReport(kind: String, lat: Double, lng: Double) {
        lifecycleScope.launch {
            try {
                YeobaekClient.api.postReport(ReportRequest(kind, lat, lng))
                awardEco(EcoStore.P_REPORT)
                loadReports()
            } catch (e: Exception) {
                Toast.makeText(this@YeobaekHomeActivity, "제보 실패: ${e.message ?: "서버 확인"}",
                    Toast.LENGTH_SHORT).show()
            }
        }
    }

    /** 반경 내 최근 제보를 지도에 마커로. */
    private fun loadReports() {
        val gmap = map ?: return
        val c = gmap.cameraPosition.target
        lifecycleScope.launch {
            try {
                val reports = YeobaekClient.api
                    .getReports(c.latitude, c.longitude, radiusKmFromMap(gmap)).reports
                reportMarkers.forEach { it.remove() }
                reportMarkers.clear()
                for (r in reports) {
                    val hue = when (r.kind) {
                        "busy" -> BitmapDescriptorFactory.HUE_RED
                        "quiet" -> BitmapDescriptorFactory.HUE_AZURE
                        else -> BitmapDescriptorFactory.HUE_VIOLET
                    }
                    val label = when (r.kind) {
                        "busy" -> "😖 붐빔 제보"; "quiet" -> "😌 한적 제보"; else -> "💡 여행 팁"
                    }
                    val m = gmap.addMarker(
                        MarkerOptions().position(LatLng(r.lat, r.lng))
                            .title(label).snippet(r.text)
                            .icon(BitmapDescriptorFactory.defaultMarker(hue)))
                    if (m != null) reportMarkers.add(m)
                }
            } catch (e: Exception) { /* 부가 기능 */ }
        }
    }

    private fun hidePlaceInfo() {
        placeInfoBehavior.state = BottomSheetBehavior.STATE_HIDDEN
    }

    private fun isPlaceInfoShown() =
        placeInfoBehavior.state != BottomSheetBehavior.STATE_HIDDEN

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
        addSelectedMarker(id, stop)
        fitCameraToSelected()
        refreshChips()
        refreshReco()   // 담은 장소는 주변 추천/핀에서 제외
        if (id > 0 && StampStore.stamp(this, id, stop.title)) {
            updateStampChip()
            Toast.makeText(this, "旅 ‘${stop.title}’ 스탬프를 모았어요", Toast.LENGTH_SHORT).show()
        }
    }

    private fun removeStop(id: Long) {
        selectedStops.remove(id)
        selectedMarkers.remove(id)?.remove()
        refreshChips()
        refreshReco()
    }

    private fun refreshChips() {
        chipGroup.removeAllViews()
        emptyHint.visibility = if (selectedStops.isEmpty()) View.VISIBLE else View.GONE
        for ((id, stop) in selectedStops) {
            val chip = Chip(this).apply {
                text = stop.title
                isCloseIconVisible = true
                setOnCloseIconClickListener { removeStop(id) }
            }
            chipGroup.addView(chip)
        }
        updateSheetSummary()
    }

    /** 접힌 시트에 보이는 요약 줄. */
    private fun updateSheetSummary() {
        sheetSummary.text = "담은 장소 ${selectedStops.size}곳"
        sheetSummarySub.text = when {
            selectedStops.isEmpty() ->
                "지도에서 장소를 담거나, 위로 올려 설정을 바꿔보세요"
            else -> selectedStops.values.joinToString(" · ") { it.title }
        }
    }

    /** 지금 보고 있는 지도 중심을 기준으로 '이 위치 코스 플래너'를 연다. */
    private fun openAreaPlanner() {
        val c = map?.cameraPosition?.target
        startActivity(Intent(this, DistrictActivity::class.java).apply {
            if (c != null) {
                putExtra(Extras.CENTER_LAT, c.latitude)
                putExtra(Extras.CENTER_LNG, c.longitude)
                putExtra(Extras.CENTER_NAME, "지도에서 고른 위치")
            }
        })
    }

    /** 담은 장소 라벨 — 브랜드 색 점 + 진한 이름으로 주변 추천과 구분. 이미 있으면 스킵. */
    private fun addSelectedMarker(id: Long, stop: Stop) {
        val gmap = map ?: return
        if (!stop.hasLatLng || selectedMarkers.containsKey(id)) return
        val brand = ContextCompat.getColor(this, R.color.ye_primary)
        val label = MapLabel.render(resources.displayMetrics, stop.title, brand, emphasize = true)
        val m = gmap.addMarker(
            MarkerOptions().position(LatLng(stop.lat, stop.lng))
                .icon(BitmapDescriptorFactory.fromBitmap(label.bitmap))
                .anchor(label.anchorX, label.anchorY)
                .zIndex(1f)   // 담은 곳은 항상 위에
        )
        if (m != null) selectedMarkers[id] = m
    }

    /** 담은 장소들이 다 보이도록 카메라를 맞춘다. */
    private fun fitCameraToSelected() {
        val gmap = map ?: return
        val pts = selectedStops.values.filter { it.hasLatLng }.map { LatLng(it.lat, it.lng) }
        when (pts.size) {
            0 -> Unit
            1 -> gmap.animateCamera(CameraUpdateFactory.newLatLngZoom(pts[0], 15f))
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
        // 시트가 접혀 있어도 상태가 보이도록 버튼 자체에 표시한다.
        planButton.text = if (loading) "만드는 중…" else "코스 만들기"
    }
}
