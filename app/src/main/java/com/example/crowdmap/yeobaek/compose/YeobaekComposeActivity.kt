package com.example.crowdmap.yeobaek.compose

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import com.example.crowdmap.yeobaek.compose.ui.DashboardRoute
import com.example.crowdmap.yeobaek.compose.ui.MapRoute
import com.example.crowdmap.yeobaek.compose.ui.ScheduleRoute

/**
 * 여백 Compose 레이어 진입점 — 기존 View 기반 화면(YeobaekHomeActivity 등)에서
 * Intent 로 실행한다:  startActivity(Intent(this, YeobaekComposeActivity::class.java))
 *
 * 데이터: 지금은 오프라인 시연을 위해 [SampleData] 를 사용한다. 실서버 연결 시
 * 각 화면을 감싸는 ViewModel 에서 YeobaekClient.api.match(...) / schedule(...) 등을
 * lifecycleScope 로 호출해 상태로 흘려보내면 된다(화면은 이미 상태를 인자로 받는 순수 함수).
 */
class YeobaekComposeActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent { YeobaekTheme { YeobaekApp() } }
    }
}

// 데모 진입 기본값(실제 앱은 검색/지도 선택값을 네비 인자로 전달).
private const val DEMO_CONTENT_ID = 900005L        // 북촌한옥마을
private const val DEMO_START_TIME = "2026-10-11T10:00:00"
private val DEMO_STOPS = listOf(900001L, 900016L, 900008L) // 경복궁·국립중앙박물관·명동
private const val DEMO_LAT = 37.5796
private const val DEMO_LNG = 126.9770

private enum class Tab(val route: String, val label: String, val glyph: String) {
    Discover("discover", "발견", "◈"),
    Schedule("schedule", "스케줄", "❙❙❙"),
    Map("map", "지도", "◍"),
}

@Composable
private fun YeobaekApp() {
    val nav = rememberNavController()
    val backStack by nav.currentBackStackEntryAsState()
    val current = backStack?.destination?.route

    Scaffold(
        bottomBar = {
            NavigationBar(containerColor = MaterialTheme.colorScheme.surface) {
                Tab.entries.forEach { tab ->
                    NavigationBarItem(
                        selected = current == tab.route,
                        onClick = {
                            nav.navigate(tab.route) {
                                popUpTo(Tab.Discover.route) { saveState = true }
                                launchSingleTop = true; restoreState = true
                            }
                        },
                        icon = { Text(tab.glyph) },
                        label = { Text(tab.label) },
                        colors = NavigationBarItemDefaults.colors(
                            selectedIconColor = YeobaekTheme.colors.brand,
                            selectedTextColor = YeobaekTheme.colors.brand,
                            indicatorColor = YeobaekTheme.colors.brandTint,
                            unselectedIconColor = YeobaekTheme.colors.inkFaint,
                            unselectedTextColor = YeobaekTheme.colors.inkFaint,
                        ),
                    )
                }
            }
        },
    ) { inner ->
        // 데모 진입 기본값. 실제 플로우에선 검색/지도에서 고른 값을 네비 인자로 넘긴다.
        // 서버 미가동 시 각 화면의 '샘플로 보기'로 오프라인 시연 가능.
        NavHost(nav, startDestination = Tab.Discover.route, modifier = Modifier.padding(inner)) {
            composable(Tab.Discover.route) {
                DashboardRoute(contentId = DEMO_CONTENT_ID)
            }
            composable(Tab.Schedule.route) {
                ScheduleRoute(startTime = DEMO_START_TIME, stops = DEMO_STOPS)
            }
            composable(Tab.Map.route) {
                MapRoute(lat = DEMO_LAT, lng = DEMO_LNG)
            }
        }
    }
}
