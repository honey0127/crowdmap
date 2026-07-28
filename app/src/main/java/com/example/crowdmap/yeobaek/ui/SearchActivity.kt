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
            hint.text = if (res.results.isEmpty()) "‘$q’에 대한 결과가 없어요" else ""
            hint.visibility = if (res.results.isEmpty()) View.VISIBLE else View.GONE
        } catch (e: Exception) {
            Toast.makeText(this, "검색 실패: ${e.message ?: "네트워크 오류"}", Toast.LENGTH_SHORT).show()
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
