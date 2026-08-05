package com.example.crowdmap.yeobaek.compose.ui

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.example.crowdmap.yeobaek.data.MatchRequest
import com.example.crowdmap.yeobaek.data.MatchResponse
import com.example.crowdmap.yeobaek.data.OffpeakResponse
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.ScheduleRequest
import com.example.crowdmap.yeobaek.data.ScheduleResponse
import com.example.crowdmap.yeobaek.data.Twin
import com.example.crowdmap.yeobaek.data.YeobaekApi
import com.example.crowdmap.yeobaek.data.YeobaekClient
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

/**
 * 여백 Compose 화면용 ViewModel — YeobaekApi(FastAPI) 를 viewModelScope 로 호출한다.
 *
 * 화면은 상태(UiState)를 인자로 받는 순수 Composable 이므로, 여기서 만든 데이터를
 * 그대로 흘려보내면 된다. 파라미터(contentId 등)는 no-arg VM + load() 로 주입한다
 * (Hilt 등 DI 없이도 동작 — 화면의 LaunchedEffect 에서 load 를 호출).
 */

private fun errMessage(e: Throwable): String = when (e) {
    is java.net.UnknownHostException, is java.net.ConnectException ->
        "서버에 연결할 수 없어요 (SERVER_IP 확인)."
    is java.net.SocketTimeoutException -> "응답이 지연됩니다. 잠시 후 다시 시도하세요."
    else -> e.message ?: "알 수 없는 오류"
}

// ── 대시보드 ──────────────────────────────────────────────
data class DashboardData(val match: MatchResponse, val sourcePct: Int, val sourceLevel: Int)

class DashboardViewModel(private val api: YeobaekApi = YeobaekClient.api) : ViewModel() {
    private val _state = MutableStateFlow<UiState<DashboardData>>(UiState.Loading)
    val state: StateFlow<UiState<DashboardData>> = _state.asStateFlow()

    fun load(contentId: Long) {
        _state.value = UiState.Loading
        viewModelScope.launch {
            try {
                val match = api.match(MatchRequest(contentId = contentId))
                // 소스 장소의 '지금' 혼잡: 좌표 있으면 resolve_now, 없으면 중립(2).
                val lvl = match.source.lat?.let { lat ->
                    match.source.lng?.let { lng ->
                        runCatching { api.resolveNow(lat, lng) }.getOrNull()?.takeIf { it.valid }?.level
                    }
                } ?: 2
                _state.value = UiState.Success(DashboardData(match, levelToPct(lvl), lvl))
            } catch (e: Throwable) {
                _state.value = UiState.Error(errMessage(e))
            }
        }
    }

    /** 오프라인 시연 폴백. */
    fun useSample(data: DashboardData) { _state.value = UiState.Success(data) }
}

// ── 스케줄 ────────────────────────────────────────────────
class ScheduleViewModel(private val api: YeobaekApi = YeobaekClient.api) : ViewModel() {
    private val _state = MutableStateFlow<UiState<ScheduleResponse>>(UiState.Loading)
    val state: StateFlow<UiState<ScheduleResponse>> = _state.asStateFlow()

    fun load(startTime: String, stops: List<Long>, allowSubstitution: Boolean = true) {
        _state.value = UiState.Loading
        viewModelScope.launch {
            try {
                _state.value = UiState.Success(
                    api.schedule(ScheduleRequest(startTime = startTime, stops = stops, allowSubstitution = allowSubstitution))
                )
            } catch (e: Throwable) {
                _state.value = UiState.Error(errMessage(e))
            }
        }
    }

    fun useSample(data: ScheduleResponse) { _state.value = UiState.Success(data) }
}

// ── 지도 히트맵 ──────────────────────────────────────────
class MapViewModel(private val api: YeobaekApi = YeobaekClient.api) : ViewModel() {
    private val _state = MutableStateFlow<UiState<List<PlaceResult>>>(UiState.Loading)
    val state: StateFlow<UiState<List<PlaceResult>>> = _state.asStateFlow()

    fun load(lat: Double, lng: Double, radiusKm: Double = 3.0) {
        _state.value = UiState.Loading
        viewModelScope.launch {
            try {
                _state.value = UiState.Success(api.heatmap(lat, lng, radiusKm).results)
            } catch (e: Throwable) {
                _state.value = UiState.Error(errMessage(e))
            }
        }
    }

    fun useSample(data: List<PlaceResult>) { _state.value = UiState.Success(data) }
}

// ── 장소 상세 (오프피크 + 분산) ──────────────────────────
data class DetailData(val place: PlaceResult, val offpeak: OffpeakResponse, val alternatives: List<Twin>)

class PlaceDetailViewModel(private val api: YeobaekApi = YeobaekClient.api) : ViewModel() {
    private val _state = MutableStateFlow<UiState<DetailData>>(UiState.Loading)
    val state: StateFlow<UiState<DetailData>> = _state.asStateFlow()

    fun load(place: PlaceResult) {
        _state.value = UiState.Loading
        viewModelScope.launch {
            try {
                val offpeak = api.offpeak(place.contentId)
                val alts = runCatching { api.disperse(place.contentId).results }.getOrDefault(emptyList())
                    .map { Twin(it.contentId, it.title, similarity = (it.quietScore ?: 0) / 100.0, forecastLevel = it.level ?: 2, distKm = it.distKm ?: 0.0) }
                _state.value = UiState.Success(DetailData(place, offpeak, alts))
            } catch (e: Throwable) {
                _state.value = UiState.Error(errMessage(e))
            }
        }
    }

    fun useSample(data: DetailData) { _state.value = UiState.Success(data) }
}

/** 레벨(1~4) → 히어로 표시용 근사 혼잡%(디자인 스케일). 서버가 실제 %를 주면 대체. */
internal fun levelToPct(level: Int): Int = when (level) { 1 -> 22; 2 -> 48; 3 -> 71; else -> 88 }
