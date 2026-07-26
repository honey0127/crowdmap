package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.ProgressBar
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.ScheduleRequest
import com.example.crowdmap.yeobaek.data.YeobaekClient
import kotlinx.coroutines.launch
import java.time.LocalDate

/**
 * 홈(절차서 4-1): 출발 시각 + 방문지(content_id) 입력 → /schedule → 플래너.
 * 예보 지평이 당일 ~12시간이므로 기본 출발시각을 오늘 오후로 프리필한다.
 */
class YeobaekHomeActivity : AppCompatActivity() {

    private lateinit var startTimeInput: EditText
    private lateinit var stopsInput: EditText
    private lateinit var progress: ProgressBar
    private lateinit var goButton: Button

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_home)

        startTimeInput = findViewById(R.id.input_start_time)
        stopsInput = findViewById(R.id.input_stops)
        progress = findViewById(R.id.home_progress)
        goButton = findViewById(R.id.btn_plan)

        // 당일 오후 출발 기본값(예보 지평 내)
        startTimeInput.setText("${LocalDate.now()}T14:00:00")
        stopsInput.setText("126508, 126521, 126540")   // 예: 경복궁/덕수궁/운현궁

        goButton.setOnClickListener { requestSchedule() }
    }

    private fun requestSchedule() {
        val startTime = startTimeInput.text.toString().trim()
        val stops = parseStops(stopsInput.text.toString())
        if (startTime.isEmpty()) {
            toast("출발 시각을 입력하세요 (예: ${LocalDate.now()}T14:00:00)"); return
        }
        if (stops.isEmpty()) {
            toast("방문지 content_id 를 하나 이상 입력하세요"); return
        }

        setLoading(true)
        lifecycleScope.launch {
            try {
                val plan = YeobaekClient.api.schedule(
                    ScheduleRequest(startTime = startTime, stops = stops)
                )
                val json = YeobaekClient.gson.toJson(plan)
                startActivity(
                    Intent(this@YeobaekHomeActivity, PlannerActivity::class.java).apply {
                        putExtra(Extras.STOPS, stops.toLongArray())
                        putExtra(Extras.START_TIME, startTime)
                        putExtra(Extras.PLAN_JSON, json)
                    }
                )
            } catch (e: Exception) {
                toast("코스 생성 실패: ${e.message ?: "네트워크 오류"} (서버 IP/실행 확인)")
            } finally {
                setLoading(false)
            }
        }
    }

    private fun parseStops(raw: String): List<Long> =
        raw.split(Regex("[^0-9]+")).mapNotNull { it.trim().toLongOrNull() }

    private fun setLoading(loading: Boolean) {
        progress.visibility = if (loading) View.VISIBLE else View.GONE
        goButton.isEnabled = !loading
    }

    private fun toast(msg: String) = Toast.makeText(this, msg, Toast.LENGTH_LONG).show()
}
