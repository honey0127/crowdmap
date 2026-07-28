package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.PlanStop
import com.example.crowdmap.yeobaek.data.ScheduleResponse
import com.example.crowdmap.yeobaek.data.YeobaekClient

/**
 * 플래너(절차서 4-2): 최적 코스 타임라인(도착시각·혼잡 배지).
 * 고혼잡 stop 의 "대안 보기" → 대안 목록(/match).
 * 스왑으로 재스케줄된 결과가 CLEAR_TOP 으로 이 액티비티를 새 인텐트로 재생성한다.
 */
class PlannerActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_planner)

        val stops = intent.getLongArrayExtra(Extras.STOPS) ?: LongArray(0)
        val startTime = intent.getStringExtra(Extras.START_TIME) ?: ""
        val planJson = intent.getStringExtra(Extras.PLAN_JSON)
        val plan: ScheduleResponse? =
            planJson?.let { YeobaekClient.gson.fromJson(it, ScheduleResponse::class.java) }

        val saved = findViewById<TextView>(R.id.planner_saved)
        val sub = findViewById<TextView>(R.id.planner_sub)
        val recycler = findViewById<RecyclerView>(R.id.planner_list)
        recycler.layoutManager = LinearLayoutManager(this)

        if (plan == null || plan.ordered.isEmpty()) {
            saved.text = "코스를 만들지 못했습니다"
            sub.text = ""
            return
        }

        saved.text = "혼잡 ${plan.savedCongestionPct}% 절약"
        sub.text = "${plan.ordered.size}곳 · 최적 비용 ${round2(plan.totalCost)}"
        recycler.adapter = PlanAdapter(plan.ordered) { stop ->
            openAlternatives(stop, stops, startTime)
        }
    }

    private fun openAlternatives(stop: PlanStop, stops: LongArray, startTime: String) {
        startActivity(
            Intent(this, AlternativesActivity::class.java).apply {
                putExtra(Extras.TARGET_ID, stop.contentId)
                putExtra(Extras.STOPS, stops)
                putExtra(Extras.START_TIME, startTime)
            }
        )
    }

    private fun round2(v: Double): String = String.format("%.2f", v)
}
