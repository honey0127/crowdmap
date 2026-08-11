package com.example.crowdmap.yeobaek.compose.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.viewmodel.compose.viewModel
import com.example.crowdmap.yeobaek.compose.DashboardScreen
import com.example.crowdmap.yeobaek.compose.MapHeatmapScreen
import com.example.crowdmap.yeobaek.compose.PlaceDetailScreen
import com.example.crowdmap.yeobaek.compose.SampleData
import com.example.crowdmap.yeobaek.compose.ScheduleScreen
import com.example.crowdmap.yeobaek.data.PlaceResult

/**
 * 상태 연결(stateful) Route — ViewModel 의 UiState 를 구독해 순수 화면에 흘려보낸다.
 * 서버 미가동/실패 시 '샘플로 보기'로 오프라인 시연을 보장한다([allowSample]).
 */

@Composable
fun DashboardRoute(contentId: Long, allowSample: Boolean = true) {
    val vm: DashboardViewModel = viewModel()
    val state by vm.state.collectAsState()
    LaunchedEffect(contentId) { vm.load(contentId) }
    StateHost(
        state = state,
        onRetry = { vm.load(contentId) },
        onUseSample = if (allowSample) {
            { vm.useSample(DashboardData(SampleData.match, SampleData.heroPct, SampleData.heroLevel)) }
        } else null,
    ) { data ->
        DashboardScreen(match = data.match, sourcePct = data.sourcePct, sourceLevel = data.sourceLevel)
    }
}

@Composable
fun ScheduleRoute(startTime: String, stops: List<Long>, allowSample: Boolean = true) {
    val vm: ScheduleViewModel = viewModel()
    val state by vm.state.collectAsState()
    val context = LocalContext.current
    LaunchedEffect(startTime, stops) { vm.load(context, startTime, stops) }
    StateHost(
        state = state,
        onRetry = { vm.load(context, startTime, stops) },
        onUseSample = if (allowSample) ({ vm.useSample(ScheduleData(SampleData.schedule)) }) else null,
    ) { data ->
        ScheduleScreen(
            schedule = data.response, dwellMin = SampleData.dwellMin, transit = SampleData.transit,
            isOffline = data.isOffline,
        )
    }
}

@Composable
fun MapRoute(lat: Double, lng: Double, allowSample: Boolean = true) {
    val vm: MapViewModel = viewModel()
    val state by vm.state.collectAsState()
    LaunchedEffect(lat, lng) { vm.load(lat, lng) }
    StateHost(
        state = state,
        onRetry = { vm.load(lat, lng) },
        onUseSample = if (allowSample) ({ vm.useSample(SampleData.heatmap) }) else null,
    ) { places ->
        MapHeatmapScreen(places = places)
    }
}

@Composable
fun PlaceDetailRoute(place: PlaceResult, allowSample: Boolean = true) {
    val vm: PlaceDetailViewModel = viewModel()
    val state by vm.state.collectAsState()
    LaunchedEffect(place.contentId) { vm.load(place) }
    StateHost(
        state = state,
        onRetry = { vm.load(place) },
        onUseSample = if (allowSample) {
            { vm.useSample(DetailData(SampleData.detailPlace, SampleData.offpeak, SampleData.match.twins)) }
        } else null,
    ) { data ->
        PlaceDetailScreen(
            place = data.place,
            offpeak = data.offpeak,
            hours = SampleData.offpeakHours,
            alternatives = data.alternatives,
        )
    }
}
