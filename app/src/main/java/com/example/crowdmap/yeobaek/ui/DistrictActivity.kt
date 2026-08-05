package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
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
 * 코스 플래너(신규): 지역구(성수·홍대 …)를 고르면 그 지역 명소 라인업이 뜨고,
 * "＋ 담기"로 내 코스에 넣어 하루 코스를 만든다. (자동 최적화가 아니라 내가 담은 순서 유지)
 */
class DistrictActivity : AppCompatActivity() {

    private val selected = LinkedHashMap<Long, PlaceResult>()   // 담은 명소(순서 보존)

    private lateinit var chipAdapter: DistrictChipAdapter
    private lateinit var lineupAdapter: LineupAdapter
    private lateinit var descView: TextView
    private lateinit var lineupProgress: ProgressBar
    private lateinit var lineupEmpty: TextView
    private lateinit var courseCount: TextView
    private lateinit var courseProgress: ProgressBar
    private lateinit var makeButton: MaterialButton

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_district)

        descView = findViewById(R.id.district_desc)
        lineupProgress = findViewById(R.id.lineup_progress)
        lineupEmpty = findViewById(R.id.lineup_empty)
        courseCount = findViewById(R.id.course_count)
        courseProgress = findViewById(R.id.course_progress)
        makeButton = findViewById(R.id.btn_make_course)

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
    }

    private fun loadDistricts() {
        lifecycleScope.launch {
            try {
                val list = YeobaekClient.api.districts().districts
                if (list.isEmpty()) return@launch
                chipAdapter.submit(list, list.first().key)
                selectDistrict(list.first())
            } catch (e: Exception) {
                Toast.makeText(this@DistrictActivity,
                    "지역 목록을 불러오지 못했어요: ${e.message ?: "서버 확인"}",
                    Toast.LENGTH_LONG).show()
            }
        }
    }

    private fun selectDistrict(d: District) {
        chipAdapter.select(d.key)
        descView.text = d.desc ?: ""
        setLineupLoading(true)
        lineupEmpty.visibility = View.GONE
        lifecycleScope.launch {
            try {
                val places = YeobaekClient.api.districtPlaces(d.key).places
                lineupAdapter.submit(places)
                lineupEmpty.visibility = if (places.isEmpty()) View.VISIBLE else View.GONE
            } catch (e: Exception) {
                lineupAdapter.submit(emptyList())
                lineupEmpty.visibility = View.VISIBLE
            } finally {
                setLineupLoading(false)
            }
        }
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
