package com.example.crowdmap.yeobaek.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.YeobaekClient
import com.google.android.material.textfield.TextInputEditText
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/** 장소 이름 검색(재설계 홈의 "장소 추가"). 300ms 디바운스 → /places/search. */
class SearchActivity : AppCompatActivity() {

    private lateinit var progress: ProgressBar
    private lateinit var hint: TextView
    private lateinit var adapter: PlaceAdapter
    private var searchJob: Job? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_yeobaek_search)

        progress = findViewById(R.id.search_progress)
        hint = findViewById(R.id.search_hint)
        val list = findViewById<RecyclerView>(R.id.search_list)
        list.layoutManager = LinearLayoutManager(this)
        adapter = PlaceAdapter { place -> returnPlace(place) }
        list.adapter = adapter

        findViewById<TextInputEditText>(R.id.search_input).doAfterTextChanged { editable ->
            val q = editable?.toString()?.trim().orEmpty()
            searchJob?.cancel()
            if (q.isEmpty()) {
                adapter.submit(emptyList())
                hint.visibility = View.VISIBLE
                return@doAfterTextChanged
            }
            searchJob = lifecycleScope.launch {
                delay(300)          // 디바운스
                doSearch(q)
            }
        }
    }

    private suspend fun doSearch(q: String) {
        setLoading(true)
        try {
            val res = YeobaekClient.api.searchPlaces(q)
            adapter.submit(res.results)
            if (res.results.isEmpty()) {
                // 결과 0건은 "검색어가 없다"기보다 "장소 DB가 비어 있다"인 경우가 많다.
                hint.text = "‘$q’에 대한 결과가 없어요.\n\n" +
                    "장소가 하나도 안 나온다면 서버에 장소 데이터가 아직 없는 상태예요.\n" +
                    "PC에서 collect_tourapi.py 를 실행해 명소를 받아오세요."
                hint.visibility = View.VISIBLE
            } else {
                hint.visibility = View.GONE
            }
        } catch (e: Exception) {
            // 네트워크/서버 문제는 조용히 지나가지 않게 화면에도 남긴다.
            adapter.submit(emptyList())
            hint.text = "서버에 연결하지 못했어요.\n\n" +
                "· 여백 서버(uvicorn)가 실행 중인지\n" +
                "· local.properties 의 DEV_BASE_URL 이 PC 주소인지\n" +
                "· 폰과 PC 가 같은 Wi-Fi 인지 확인해 주세요.\n\n" +
                "(${e.message ?: "네트워크 오류"})"
            hint.visibility = View.VISIBLE
            Toast.makeText(this, "검색 실패: 서버 연결 확인", Toast.LENGTH_SHORT).show()
        } finally {
            setLoading(false)
        }
    }

    private fun returnPlace(place: PlaceResult) {
        setResult(RESULT_OK, Intent().apply {
            putExtra(Extras.PLACE_ID, place.contentId)
            putExtra(Extras.PLACE_TITLE, place.title)
            putExtra(Extras.PLACE_LAT, place.lat ?: Double.NaN)
            putExtra(Extras.PLACE_LNG, place.lng ?: Double.NaN)
        })
        finish()
    }

    private fun setLoading(loading: Boolean) {
        progress.visibility = if (loading) View.VISIBLE else View.GONE
    }
}
