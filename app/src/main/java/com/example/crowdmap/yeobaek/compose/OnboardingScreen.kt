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
import androidx.compose.foundation.layout.width
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
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.R
import com.example.crowdmap.yeobaek.compose.components.clickableNoRipple
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import kotlinx.coroutines.launch

/** [eyebrow]/[title]/[body] 는 strings.xml 리소스 ID — 로케일(values-en 등)에 따라 자동 번역. */
private data class OnboardPage(val glyph: String, val eyebrow: Int, val title: Int, val body: Int)

private val pages = listOf(
    OnboardPage("餘", R.string.onboard1_eyebrow, R.string.onboard1_title, R.string.onboard1_body),
    OnboardPage("◐", R.string.onboard2_eyebrow, R.string.onboard2_title, R.string.onboard2_body),
    OnboardPage("⇄", R.string.onboard3_eyebrow, R.string.onboard3_title, R.string.onboard3_body),
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
            Text(stringResource(R.string.onboard_skip), color = YeobaekTheme.colors.inkFaint, fontSize = 13.sp, fontWeight = FontWeight.Medium, modifier = Modifier.clickableNoRipple(onFinish))
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
            Text(
                stringResource(if (isLast) R.string.onboard_start else R.string.onboard_next),
                color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.SemiBold,
            )
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
        Text(stringResource(page.eyebrow).uppercase(), color = YeobaekTheme.colors.brand, fontSize = 12.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.8.sp)
        Spacer(Modifier.height(14.dp))
        Text(
            stringResource(page.title), style = MaterialTheme.typography.displaySmall, color = MaterialTheme.colorScheme.onBackground,
            textAlign = TextAlign.Center, lineHeight = 40.sp,
        )
        Spacer(Modifier.height(16.dp))
        Text(
            stringResource(page.body), color = YeobaekTheme.colors.inkSlate, fontSize = 15.sp, textAlign = TextAlign.Center, lineHeight = 23.sp,
        )
    }
}

@Preview(showBackground = true, heightDp = 780)
@Composable
private fun OnboardingPreview() {
    YeobaekTheme { Surface { OnboardingScreen() } }
}
