package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.Congestion
import com.example.crowdmap.yeobaek.data.PlanStop
import com.example.crowdmap.yeobaek.data.ScheduleResponse
import com.example.crowdmap.yeobaek.data.YeobaekClient
import com.google.android.material.card.MaterialCardView
import kotlinx.coroutines.launch

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
        val keepOrder = intent.getBooleanExtra(Extras.KEEP_ORDER, false)
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

        if (keepOrder) {
            saved.text = "내 순서대로 코스"
            sub.text = "${plan.ordered.size}곳 · 고른 순서 유지 · 도착 시점 혼잡도 표시"
        } else {
            saved.text = "혼잡 ${plan.savedCongestionPct}% 절약"
            sub.text = "${plan.ordered.size}곳 · 혼잡도 예측으로 순서 자동 조정"
        }

        // 역방향/엇갈림 동선 힌트(모듈: counter-flow) — 자동 재배치로 혼잡을 회피했을 때
        val hint = findViewById<TextView>(R.id.planner_hint)
        if (!keepOrder && plan.savedCongestionPct > 0) {
            hint.visibility = View.VISIBLE
            hint.text = "↺ 군중과 엇갈리는 동선으로 재배치했어요 (혼잡 ${plan.savedCongestionPct}%↓)"
        } else {
            hint.visibility = View.GONE
        }

        recycler.adapter = PlanAdapter(plan.ordered) { stop ->
            openAlternatives(stop, stops, startTime)
        }

        checkRealtimeSurge(plan)
    }

    /** 실시간 급증 알림(모듈4): 코스 상의 장소 중 지금 붐비는 곳을 감지해 배너로. */
    private fun checkRealtimeSurge(plan: ScheduleResponse) {
        val alert = findViewById<MaterialCardView>(R.id.planner_alert)
        val alertText = findViewById<TextView>(R.id.planner_alert_text)
        lifecycleScope.launch {
            for (stop in plan.ordered) {
                val lat = stop.lat ?: continue
                val lng = stop.lng ?: continue
                val now = try {
                    YeobaekClient.api.resolveNow(lat, lng)
                } catch (e: Exception) { null } ?: continue
                val lvl = now.level
                if (now.valid && lvl != null && lvl >= 3) {
                    alertText.text =
                        "⚠ ‘${stop.title}’ 지금 ${Congestion.label(lvl)} — 도착 시점을 늦추거나 대안을 눌러보세요"
                    alert.visibility = View.VISIBLE
                    return@launch   // 첫 급증 지점만 알림
                }
            }
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
