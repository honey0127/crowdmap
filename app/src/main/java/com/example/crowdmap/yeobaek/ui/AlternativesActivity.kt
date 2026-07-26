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
import com.example.crowdmap.yeobaek.data.MatchRequest
import com.example.crowdmap.yeobaek.data.Twin
import com.example.crowdmap.yeobaek.data.YeobaekClient
import kotlinx.coroutines.launch

/**
 * 대안 목록(절차서 4-3의 진입): 고혼잡 stop → /match 감성 쌍둥이 top-K.
 * arrival_time 을 코스 출발 시각으로 넘겨 같은 시간대 예보를 반영한다.
 */
class AlternativesActivity : AppCompatActivity() {

    private lateinit var progress: ProgressBar
    private lateinit var header: TextView
    private lateinit var recycler: RecyclerView

    private var targetId: Long = 0
    private lateinit var stops: LongArray
    private lateinit var startTime: String

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_alternatives)

        targetId = intent.getLongExtra(Extras.TARGET_ID, 0)
        stops = intent.getLongArrayExtra(Extras.STOPS) ?: LongArray(0)
        startTime = intent.getStringExtra(Extras.START_TIME) ?: ""

        progress = findViewById(R.id.alt_progress)
        header = findViewById(R.id.alt_header)
        recycler = findViewById(R.id.alt_list)
        recycler.layoutManager = LinearLayoutManager(this)

        loadTwins()
    }

    private fun loadTwins() {
        setLoading(true)
        lifecycleScope.launch {
            try {
                val res = YeobaekClient.api.match(
                    MatchRequest(contentId = targetId, arrivalTime = startTime)
                )
                header.text = "‘${res.source.title}’ 대신 가볼 만한 곳"
                if (res.twins.isEmpty()) {
                    header.text = "‘${res.source.title}’의 대안을 찾지 못했습니다"
                }
                recycler.adapter = TwinAdapter(res.twins) { twin -> openCard(twin) }
            } catch (e: Exception) {
                Toast.makeText(this@AlternativesActivity,
                    "대안 조회 실패: ${e.message ?: "네트워크 오류"}", Toast.LENGTH_LONG).show()
            } finally {
                setLoading(false)
            }
        }
    }

    private fun openCard(twin: Twin) {
        startActivity(
            Intent(this, CardActivity::class.java).apply {
                putExtra(Extras.SOURCE_ID, targetId)
                putExtra(Extras.ALT_ID, twin.contentId)
                putExtra(Extras.STOPS, stops)
                putExtra(Extras.START_TIME, startTime)
            }
        )
    }

    private fun setLoading(loading: Boolean) {
        progress.visibility = if (loading) View.VISIBLE else View.GONE
    }
}
