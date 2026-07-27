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
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.data.ScheduleRequest
import com.example.crowdmap.yeobaek.data.YeobaekClient
import com.google.android.material.button.MaterialButton
import com.google.android.material.chip.Chip
import com.google.android.material.chip.ChipGroup
import com.google.android.material.datepicker.MaterialDatePicker
import com.google.android.material.textfield.TextInputEditText
import com.google.android.material.timepicker.MaterialTimePicker
import com.google.android.material.timepicker.TimeFormat
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.LocalTime
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale

/**
 * 홈(재설계, 절차서 4-1): 날짜/시간 피커 + 이름으로 장소 검색·추가(칩) → /schedule.
 * 예보 지평이 당일 ~12시간이므로 기본값을 오늘 오후로 둔다.
 */
class YeobaekHomeActivity : AppCompatActivity() {

    private val selectedStops = LinkedHashMap<Long, String>()  // id → title (순서 보존)
    private var date: LocalDate = LocalDate.now()
    private var time: LocalTime = LocalTime.of(14, 0)

    private lateinit var dateField: TextInputEditText
    private lateinit var timeField: TextInputEditText
    private lateinit var chipGroup: ChipGroup
    private lateinit var emptyHint: TextView
    private lateinit var progress: ProgressBar
    private lateinit var planButton: MaterialButton

    private val searchLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode == RESULT_OK) {
            val id = result.data?.getLongExtra(Extras.PLACE_ID, -1L) ?: -1L
            val title = result.data?.getStringExtra(Extras.PLACE_TITLE) ?: return@registerForActivityResult
            if (id > 0) addStop(id, title)
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

        dateField.setOnClickListener { pickDate() }
        timeField.setOnClickListener { pickTime() }
        findViewById<MaterialButton>(R.id.btn_add_place).setOnClickListener {
            searchLauncher.launch(Intent(this, SearchActivity::class.java))
        }
        planButton.setOnClickListener { requestSchedule() }

        updateDateText()
        updateTimeText()
        refreshChips()
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

    private fun addStop(id: Long, title: String) {
        if (selectedStops.containsKey(id)) {
            Toast.makeText(this, "이미 추가된 장소예요", Toast.LENGTH_SHORT).show(); return
        }
        selectedStops[id] = title
        refreshChips()
    }

    private fun refreshChips() {
        chipGroup.removeAllViews()
        emptyHint.visibility = if (selectedStops.isEmpty()) View.VISIBLE else View.GONE
        for ((id, title) in selectedStops) {
            val chip = Chip(this).apply {
                text = title
                isCloseIconVisible = true
                setOnCloseIconClickListener { selectedStops.remove(id); refreshChips() }
            }
            chipGroup.addView(chip)
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
                val plan = YeobaekClient.api.schedule(ScheduleRequest(startTime = startTime, stops = stops))
                val json = YeobaekClient.gson.toJson(plan)
                startActivity(
                    Intent(this@YeobaekHomeActivity, PlannerActivity::class.java).apply {
                        putExtra(Extras.STOPS, stops.toLongArray())
                        putExtra(Extras.START_TIME, startTime)
                        putExtra(Extras.PLAN_JSON, json)
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
    }
}
