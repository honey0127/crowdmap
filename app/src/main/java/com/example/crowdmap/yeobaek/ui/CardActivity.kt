package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.CardRequest
import com.example.crowdmap.yeobaek.data.ScheduleRequest
import com.example.crowdmap.yeobaek.data.YeobaekClient
import kotlinx.coroutines.launch

/**
 * 대안카드(절차서 4-3): /card 근거 문구 + 혼잡도 비교 + 원탭 스왑(모듈4).
 * 스왑 시 해당 stop 을 대안으로 교체해 /schedule 재호출 → CLEAR_TOP 으로 플래너 갱신.
 */
class CardActivity : AppCompatActivity() {

    private lateinit var progress: ProgressBar
    private lateinit var headline: TextView
    private lateinit var body: TextView
    private lateinit var facts: TextView
    private lateinit var genBy: TextView
    private lateinit var swapBtn: Button

    private var sourceId: Long = 0
    private var altId: Long = 0
    private lateinit var stops: LongArray
    private lateinit var startTime: String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_card)

        sourceId = intent.getLongExtra(Extras.SOURCE_ID, 0)
        altId = intent.getLongExtra(Extras.ALT_ID, 0)
        stops = intent.getLongArrayExtra(Extras.STOPS) ?: LongArray(0)
        startTime = intent.getStringExtra(Extras.START_TIME) ?: ""

        progress = findViewById(R.id.card_progress)
        headline = findViewById(R.id.card_headline)
        body = findViewById(R.id.card_body)
        facts = findViewById(R.id.card_facts)
        genBy = findViewById(R.id.card_genby)
        swapBtn = findViewById(R.id.card_swap)
        swapBtn.isEnabled = false

        loadCard()
        swapBtn.setOnClickListener { swapAndReschedule() }
    }

    private fun loadCard() {
        setLoading(true)
        lifecycleScope.launch {
            try {
                val c = YeobaekClient.api.card(
                    CardRequest(sourceId = sourceId, altId = altId, arrivalTime = startTime)
                )
                headline.text = c.headline
                body.text = c.body
                val cat = c.groundedFacts.sharedCategory?.let { " · 공유 ‘$it’" } ?: ""
                facts.text = "혼잡도 ${c.groundedFacts.congestionDiffPct}% 낮음$cat"
                genBy.text = if (c.generatedBy == "llm") "LLM 재서술" else "근거 기반 문구"
                swapBtn.isEnabled = true
            } catch (e: Exception) {
                Toast.makeText(this@CardActivity,
                    "카드 조회 실패: ${e.message ?: "네트워크 오류"}", Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
            }
        }
    }

    private fun swapAndReschedule() {
        val newStops = stops.map { if (it == sourceId) altId else it }
        setLoading(true)
        swapBtn.isEnabled = false
        lifecycleScope.launch {
            try {
                val plan = YeobaekClient.api.schedule(
                    ScheduleRequest(startTime = startTime, stops = newStops)
                )
                val json = YeobaekClient.gson.toJson(plan)
                // CLEAR_TOP: 스택의 기존 PlannerActivity 를 새 인텐트로 재생성하고 그 위(대안/카드)는 정리
                startActivity(
                    Intent(this@CardActivity, PlannerActivity::class.java).apply {
                        addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP)
                        putExtra(Extras.STOPS, newStops.toLongArray())
                        putExtra(Extras.START_TIME, startTime)
                        putExtra(Extras.PLAN_JSON, json)
                    }
                )
                finish()
            } catch (e: Exception) {
                Toast.makeText(this@CardActivity,
                    "스왑 재계획 실패: ${e.message ?: "네트워크 오류"}", Toast.LENGTH_LONG).show()
                swapBtn.isEnabled = true
            } finally {
                setLoading(false)
            }
        }
    }

    private fun setLoading(loading: Boolean) {
        progress.visibility = if (loading) View.VISIBLE else View.GONE
    }
}
