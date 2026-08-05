package com.example.crowdmap.yeobaek.compose

import androidx.compose.animation.core.animateDpAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.components.clickableNoRipple
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import kotlinx.coroutines.launch

private data class OnboardPage(val glyph: String, val eyebrow: String, val title: String, val body: String)

private val pages = listOf(
    OnboardPage("餘", "여백 · 旅白", "여백을 두고\n여행하세요", "그림의 여백처럼, 여행에도 비움의 여유가 필요합니다. 붐빔을 피해 쉼표가 있는 하루를 설계해요."),
    OnboardPage("◐", "실시간 혼잡 예보", "붐비기 전에\n먼저 알아요", "서울 121개 예보지점의 시간대별 혼잡을 읽어, 지금 이 순간 한적한 곳을 알려드립니다."),
    OnboardPage("⇄", "감성 쌍둥이", "같은 감성,\n다른 한적함", "붐비는 명소 대신 값어치가 같은 조용한 대안으로. C++ 엔진이 하루 동선을 다시 짭니다."),
)

/**
 * 화면 5 · 온보딩 — 3페이지로 여백의 컨셉·가치를 소개.
 */
@Composable
fun OnboardingScreen(
    modifier: Modifier = Modifier,
    onFinish: () -> Unit = {},
) {
    val state = rememberPagerState(pageCount = { pages.size })
    val scope = rememberCoroutineScope()
    val isLast = state.currentPage == pages.lastIndex

    Column(modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)) {
        Row(Modifier.fillMaxWidth().padding(16.dp)) {
            Spacer(Modifier.weight(1f))
            Text("건너뛰기", color = YeobaekTheme.colors.inkFaint, fontSize = 13.sp, fontWeight = FontWeight.Medium, modifier = Modifier.clickableNoRipple(onFinish))
        }

        HorizontalPager(state = state, modifier = Modifier.weight(1f)) { page ->
            OnboardPane(pages[page])
        }

        // 인디케이터
        Row(
            Modifier.fillMaxWidth().padding(bottom = 8.dp),
            horizontalArrangement = Arrangement.Center,
        ) {
            repeat(pages.size) { i ->
                val w by animateDpAsState(if (i == state.currentPage) 22.dp else 7.dp, label = "dot")
                Box(
                    Modifier.padding(horizontal = 3.dp).height(7.dp).width(w)
                        .clip(RoundedCornerShape(999.dp))
                        .background(if (i == state.currentPage) YeobaekTheme.colors.brand else YeobaekTheme.colors.hairline)
                )
            }
        }

        // CTA
        Box(
            Modifier.fillMaxWidth().padding(16.dp).clip(RoundedCornerShape(999.dp))
                .background(YeobaekTheme.colors.brand)
                .clickableNoRipple {
                    if (isLast) onFinish() else scope.launch { state.animateScrollToPage(state.currentPage + 1) }
                }
                .padding(vertical = 16.dp),
            contentAlignment = Alignment.Center,
        ) {
            Text(if (isLast) "여백 시작하기" else "다음", color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}

@Composable
private fun OnboardPane(page: OnboardPage) {
    Column(
        Modifier.fillMaxSize().padding(horizontal = 32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Box(
            Modifier.size(96.dp).clip(RoundedCornerShape(28.dp))
                .background(Brush.linearGradient(listOf(YeobaekTheme.colors.brand, YeobaekTheme.colors.brandDeep))),
            contentAlignment = Alignment.Center,
        ) { Text(page.glyph, color = Color(0xFFEAF6F4), fontSize = 40.sp, fontWeight = FontWeight.Bold) }

        Spacer(Modifier.height(34.dp))
        Text(page.eyebrow.uppercase(), color = YeobaekTheme.colors.brand, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.8.sp)
        Spacer(Modifier.height(14.dp))
        Text(
            page.title, style = MaterialTheme.typography.displaySmall, color = MaterialTheme.colorScheme.onBackground,
            textAlign = TextAlign.Center, lineHeight = 40.sp,
        )
        Spacer(Modifier.height(16.dp))
        Text(
            page.body, color = YeobaekTheme.colors.inkSlate, fontSize = 15.sp, textAlign = TextAlign.Center, lineHeight = 23.sp,
        )
    }
}

@Preview(showBackground = true, heightDp = 780)
@Composable
private fun OnboardingPreview() {
    YeobaekTheme { Surface { OnboardingScreen() } }
}
