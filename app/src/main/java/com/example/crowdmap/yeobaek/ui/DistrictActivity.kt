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
import com.example.crowdmap.yeobaek.data.District
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.ScheduleRequest
import com.example.crowdmap.yeobaek.data.YeobaekClient
import com.google.android.material.button.MaterialButton
import kotlinx.coroutines.launch
import java.time.LocalDateTime
import java.time.format.DateTimeFormatter

/**
 * 위치 기준 코스 플래너.
 *
 * 사용자가 **기준 위치**를 고르면(장소 검색 / 빠른선택 프리셋 / 홈 지도 중심에서 진입)
 * 그 좌표 주변 명소 라인업을 보여주고, 담은 순서대로 하루 코스를 만든다.
 * 라인업은 /places/heatmap 을 써서 각 명소의 현재 혼잡·한적함까지 함께 보여준다.
 */
class DistrictActivity : AppCompatActivity() {

    private val selected = LinkedHashMap<Long, PlaceResult>()   // 담은 명소(순서 보존)

    private lateinit var chipAdapter: DistrictChipAdapter
    private lateinit var lineupAdapter: LineupAdapter
    private lateinit var centerName: TextView
    private lateinit var descView: TextView
    private lateinit var lineupProgress: ProgressBar
    private lateinit var lineupEmpty: TextView
    private lateinit var courseCount: TextView
    private lateinit var courseProgress: ProgressBar
    private lateinit var makeButton: MaterialButton

    /** 현재 기준 위치. 없으면 아직 아무것도 안 고른 상태. */
    private var centerLat: Double? = null
    private var centerLng: Double? = null
    private var radiusKm = 3.0

    /** 장소 검색으로 기준 위치 바꾸기. */
    private val pickCenterLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != RESULT_OK) return@registerForActivityResult
        val d = result.data ?: return@registerForActivityResult
        val lat = d.getDoubleExtra(Extras.PLACE_LAT, Double.NaN)
        val lng = d.getDoubleExtra(Extras.PLACE_LNG, Double.NaN)
        val title = d.getStringExtra(Extras.PLACE_TITLE) ?: "선택한 위치"
        if (lat.isNaN() || lng.isNaN()) {
            Toast.makeText(this, "그 장소는 좌표가 없어 기준으로 쓸 수 없어요", Toast.LENGTH_SHORT).show()
            return@registerForActivityResult
        }
        chipAdapter.select("")           // 프리셋 선택 해제
        setCenter(title, lat, lng, desc = "‘$title’ 주변 ${fmtRadius()} 명소")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_district)

        centerName = findViewById(R.id.center_name)
        descView = findViewById(R.id.district_desc)
        lineupProgress = findViewById(R.id.lineup_progress)
        lineupEmpty = findViewById(R.id.lineup_empty)
        courseCount = findViewById(R.id.course_count)
        courseProgress = findViewById(R.id.course_progress)
        makeButton = findViewById(R.id.btn_make_course)

        findViewById<View>(R.id.center_bar).setOnClickListener {
            pickCenterLauncher.launch(Intent(this, SearchActivity::class.java))
        }

        chipAdapter = DistrictChipAdapter { d -> selectDistrict(d) }
        findViewById<RecyclerView>(R.id.district_list).apply {
            layoutManager = LinearLayoutManager(this@DistrictActivity,
                LinearLayoutManager.HORIZONTAL, false)
            adapter = chipAdapter
        }

        lineupAdapter = LineupAdapter(
            isSelected = { id -> selected.containsKey(id) },
            onToggle = { p -> toggle(p) },
        )
        findViewById<RecyclerView>(R.id.lineup_list).apply {
            layoutManager = LinearLayoutManager(this@DistrictActivity)
            adapter = lineupAdapter
        }

        makeButton.setOnClickListener { makeCourse() }
        updateCount()
        loadDistricts()

        // 홈 지도에서 "이 위치 기준으로" 로 들어온 경우 그 좌표로 시작.
        val lat = intent.getDoubleExtra(Extras.CENTER_LAT, Double.NaN)
        val lng = intent.getDoubleExtra(Extras.CENTER_LNG, Double.NaN)
        if (!lat.isNaN() && !lng.isNaN()) {
            val name = intent.getStringExtra(Extras.CENTER_NAME) ?: "지도에서 고른 위치"
            setCenter(name, lat, lng, desc = "지도 중심 주변 ${fmtRadius()} 명소")
        } else {
            showEmpty("위쪽 ‘기준 위치’를 눌러 장소를 검색하거나,\n아래 빠른 선택에서 지역을 골라보세요.")
        }
    }

    private fun fmtRadius() = "${radiusKm.toInt()}km"

    /** 빠른선택 프리셋(성수·홍대 …)을 불러온다. 실패해도 위치 검색은 계속 쓸 수 있다. */
    private fun loadDistricts() {
        lifecycleScope.launch {
            try {
                val list = YeobaekClient.api.districts().districts
                if (list.isEmpty()) return@launch
                chipAdapter.submit(list, "")
            } catch (e: Exception) {
                // 프리셋은 부가 기능 — 검색으로 위치를 고를 수 있으니 조용히 넘어간다.
            }
        }
    }

    private fun selectDistrict(d: District) {
        chipAdapter.select(d.key)
        setCenter(d.name, d.lat, d.lng, desc = d.desc ?: "${d.name} 주변 ${fmtRadius()} 명소")
    }

    /** 기준 위치를 정하고 그 주변 라인업을 불러온다. */
    private fun setCenter(name: String, lat: Double, lng: Double, desc: String) {
        centerLat = lat
        centerLng = lng
        centerName.text = name
        descView.text = desc
        loadLineup(lat, lng)
    }

    private fun loadLineup(lat: Double, lng: Double) {
        setLineupLoading(true)
        lineupEmpty.visibility = View.GONE
        lifecycleScope.launch {
            try {
                // 주변 명소 + 현재 혼잡/한적함(핀 색과 동일한 소스)
                val places = YeobaekClient.api.heatmap(lat, lng, radiusKm, limit = 30).results
                lineupAdapter.submit(places)
                if (places.isEmpty()) showEmpty(
                    "이 위치 반경 ${fmtRadius()} 안에 등록된 명소가 없어요.\n" +
                        "다른 위치를 고르거나, 홈 지도에서 직접 장소를 담아보세요."
                ) else lineupEmpty.visibility = View.GONE
            } catch (e: Exception) {
                lineupAdapter.submit(emptyList())
                showEmpty(
                    "명소를 불러오지 못했어요.\n" +
                        "서버가 켜져 있는지, SERVER_IP 설정이 맞는지 확인해 주세요.\n" +
                        "(${e.message ?: "네트워크 오류"})"
                )
            } finally {
                setLineupLoading(false)
            }
        }
    }

    private fun showEmpty(msg: String) {
        lineupEmpty.text = msg
        lineupEmpty.visibility = View.VISIBLE
    }

    private fun toggle(p: PlaceResult) {
        if (selected.containsKey(p.contentId)) selected.remove(p.contentId)
        else selected[p.contentId] = p
        lineupAdapter.notifyDataSetChanged()
        updateCount()
    }

    private fun updateCount() {
        courseCount.text = "내 코스 ${selected.size}곳"
        makeButton.isEnabled = selected.isNotEmpty()
    }

    private fun makeCourse() {
        if (selected.isEmpty()) return
        val startTime = LocalDateTime.now()
            .format(DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:00"))
        val stops = selected.keys.toList()

        setCourseLoading(true)
        lifecycleScope.launch {
            try {
                // 내가 담은 순서를 유지(자동 재배치 X) — 라인업 그대로 하루 코스로.
                val plan = YeobaekClient.api.schedule(
                    ScheduleRequest(startTime = startTime, stops = stops, keepOrder = true))
                val json = YeobaekClient.gson.toJson(plan)
                startActivity(
                    Intent(this@DistrictActivity, PlannerActivity::class.java).apply {
                        putExtra(Extras.STOPS, stops.toLongArray())
                        putExtra(Extras.START_TIME, startTime)
                        putExtra(Extras.PLAN_JSON, json)
                        putExtra(Extras.KEEP_ORDER, true)
                    })
            } catch (e: Exception) {
                Toast.makeText(this@DistrictActivity,
                    "코스 생성 실패: ${e.message ?: "네트워크 오류"}", Toast.LENGTH_LONG).show()
            } finally {
                setCourseLoading(false)
            }
        }
    }

    private fun setLineupLoading(loading: Boolean) {
        lineupProgress.visibility = if (loading) View.VISIBLE else View.GONE
    }

    private fun setCourseLoading(loading: Boolean) {
        courseProgress.visibility = if (loading) View.VISIBLE else View.GONE
        makeButton.isEnabled = !loading && selected.isNotEmpty()
    }
}
