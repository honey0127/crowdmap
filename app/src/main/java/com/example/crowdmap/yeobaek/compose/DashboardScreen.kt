package com.example.crowdmap.yeobaek.compose

import androidx.compose.animation.core.animateIntAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.components.DensityMeter
import com.example.crowdmap.yeobaek.compose.components.DensityPill
import com.example.crowdmap.yeobaek.compose.components.SaveChip
import com.example.crowdmap.yeobaek.compose.components.SectionHeader
import com.example.crowdmap.yeobaek.compose.components.clickableNoRipple
import com.example.crowdmap.yeobaek.compose.components.hairline
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import com.example.crowdmap.yeobaek.compose.theme.densityLabel
import com.example.crowdmap.yeobaek.data.MatchResponse
import com.example.crowdmap.yeobaek.data.Twin

/** 레벨(1~4) → 히어로 표시용 근사 혼잡%(디자인 스케일). 실제 %는 서버 확장 시 대체. */
private fun levelToPct(level: Int): Int = when (level) { 1 -> 22; 2 -> 48; 3 -> 71; else -> 88 }

/**
 * 화면 1 · 여백의 발견 — 메인 대시보드.
 * 글래스 혼잡 히어로 + 감성 쌍둥이 Alt-Route 스왑 카드.
 */
@Composable
fun DashboardScreen(
    match: MatchResponse,
    sourcePct: Int,
    sourceLevel: Int,
    modifier: Modifier = Modifier,
    onApply: (Twin) -> Unit = {},
) {
    val twin = match.twins.firstOrNull()
    var swapped by remember { mutableStateOf(false) }

    val shownPct by animateIntAsState(
        targetValue = if (swapped && twin != null) levelToPct(twin.forecastLevel) else sourcePct,
        animationSpec = tween(460), label = "heroPct",
    )
    val shownLevel = if (swapped && twin != null) twin.forecastLevel else sourceLevel
    val shownTitle = if (swapped && twin != null) twin.title else match.source.title

    Column(
        modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp),
    ) {
        DashboardTopBar(place = match.source.title)

        GlassHero(
            pct = shownPct,
            level = shownLevel,
            subtitle = "$shownTitle · ${match.source.cat ?: "명소"}",
        )

        Spacer(Modifier.height(22.dp))
        SectionHeader(title = "여백이 있는 대안", action = "모두 보기 →")
        Spacer(Modifier.height(10.dp))

        if (twin != null) {
            AltRouteCard(
                source = match.source.title,
                sourceCat = match.source.cat ?: "명소",
                sourceLevel = sourceLevel,
                twin = twin,
                swapped = swapped,
                onSwap = { swapped = !swapped },
                onApply = { onApply(twin) },
            )
            Spacer(Modifier.height(14.dp))
            Text(
                "두 곳 모두 ${match.source.cat ?: "같은"} · e5 감성 유사도 ${"%.2f".format(twin.similarity)} — 값어치는 같고, 붐빔만 다릅니다.",
                color = YeobaekTheme.colors.inkSlate,
                fontSize = 11.5f.sp,
                lineHeight = 16.sp,
                modifier = Modifier.fillMaxWidth().padding(horizontal = 4.dp),
            )
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun DashboardTopBar(place: String) {
    Row(
        Modifier.fillMaxWidth().padding(top = 14.dp, bottom = 12.dp),
        verticalAlignment = Alignment.Top,
    ) {
        Column {
            Text("화요일 오전 · 여행 2일차", color = YeobaekTheme.colors.inkFaint, fontSize = 12.sp, fontWeight = FontWeight.Medium)
            Text(place, style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(top = 3.dp))
        }
        Spacer(Modifier.weight(1f))
        Box(
            Modifier.size(34.dp).clip(MaterialTheme.shapes.small)
                .background(YeobaekTheme.colors.brandTint)
                .hairline(MaterialTheme.shapes.small),
            contentAlignment = Alignment.Center,
        ) { Text("旅", color = YeobaekTheme.colors.brand, fontWeight = FontWeight.Bold, fontSize = 15.sp) }
    }
}

/** 글래스 혼잡 히어로 — 브랜드 그라디언트 위 backdrop 상태 배지 + 세그먼트 미터. */
@Composable
private fun GlassHero(pct: Int, level: Int, subtitle: String) {
    val brush = YeobaekTheme.colors.heroBrush
    Box(
        Modifier
            .fillMaxWidth()
            .clip(MaterialTheme.shapes.large)
            .background(brush)
            .padding(20.dp),
    ) {
        Column {
            Text("◷ 지금 · 오전 10:00 도착 기준", color = Color(0xCCEAF6F4), fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    "$pct",
                    color = Color(0xFFEAF6F4),
                    fontSize = 46.sp,
                    fontWeight = FontWeight.W800,
                    style = MaterialTheme.typography.displaySmall.copy(
                        fontFeatureSettings = "tnum",
                        fontSize = 46.sp, letterSpacing = (-2).sp,
                    ),
                )
                Text("%", color = Color(0xCCEAF6F4), fontSize = 16.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(bottom = 7.dp, start = 2.dp))
                Spacer(Modifier.weight(1f))
                GlassStatePill(level)
            }
            Spacer(Modifier.height(16.dp))
            DensityMeter(ratio = pct / 100f)
            Spacer(Modifier.height(11.dp))
            Row {
                Text(subtitle, color = Color(0xCCEAF6F4), fontSize = 11.5f.sp, fontWeight = FontWeight.Medium)
                Spacer(Modifier.weight(1f))
                Text("여백 지수 ▾", color = Color(0xB3EAF6F4), fontSize = 11.5f.sp)
            }
        }
    }
}

@Composable
private fun GlassStatePill(level: Int) {
    Row(
        Modifier
            .clip(androidx.compose.foundation.shape.RoundedCornerShape(999.dp))
            .background(Color(0x29FFFFFF))
            .padding(horizontal = 13.dp, vertical = 7.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        val dot = if (level >= 3) Color(0xFFF2B8B9) else Color(0xFF8FE3BE)
        Box(Modifier.size(8.dp).clip(CircleShape).background(dot))
        Text("${densityLabel(level)} 시간대", color = Color(0xFFEAF6F4), fontSize = 12.5f.sp, fontWeight = FontWeight.SemiBold)
    }
}

/** Alt-Route 비교 카드 — 좌 크림슨(붐빔) · 우 세이지(한적) + 중앙 스왑 + 절약/적용. */
@Composable
private fun AltRouteCard(
    source: String,
    sourceCat: String,
    sourceLevel: Int,
    twin: Twin,
    swapped: Boolean,
    onSwap: () -> Unit,
    onApply: () -> Unit,
) {
    Column(
        Modifier
            .fillMaxWidth()
            .clip(MaterialTheme.shapes.large)
            .background(MaterialTheme.colorScheme.surface)
            .hairline(MaterialTheme.shapes.large),
    ) {
        Row(Modifier.height(IntrinsicSize.Max)) {
            AltColumn(
                modifier = Modifier.weight(1f).fillMaxHeight(),
                tag = "지금 붐빔",
                tagColor = YeobaekTheme.colors.density(4).ink,
                bg = YeobaekTheme.colors.density(sourceLevel).bg,
                name = source,
                cat = sourceCat,
                pct = levelToPct(sourceLevel),
                pctColor = YeobaekTheme.colors.density(sourceLevel).fg,
                level = sourceLevel,
            )
            // 중앙 스왑
            Box(
                Modifier.width(46.dp).fillMaxHeight()
                    .background(MaterialTheme.colorScheme.surface),
                contentAlignment = Alignment.Center,
            ) {
                Box(
                    Modifier.size(38.dp).clip(CircleShape)
                        .background(if (swapped) YeobaekTheme.colors.brand else MaterialTheme.colorScheme.surface)
                        .hairline(CircleShape)
                        .clickableNoRipple(onSwap),
                    contentAlignment = Alignment.Center,
                ) {
                    Text("⇄", color = if (swapped) Color.White else YeobaekTheme.colors.brand, fontSize = 17.sp, fontWeight = FontWeight.Bold)
                }
            }
            AltColumn(
                modifier = Modifier.weight(1f).fillMaxHeight(),
                tag = "추천 · 한적",
                tagColor = YeobaekTheme.colors.density(1).ink,
                bg = YeobaekTheme.colors.density(twin.forecastLevel).bg,
                name = twin.title,
                cat = "유사도 ${"%.2f".format(twin.similarity)} · ${"%.1f".format(twin.distKm)}km",
                pct = levelToPct(twin.forecastLevel),
                pctColor = YeobaekTheme.colors.density(twin.forecastLevel).fg,
                level = twin.forecastLevel,
            )
        }
        // 하단 절약/적용
        Row(
            Modifier.fillMaxWidth()
                .background(MaterialTheme.colorScheme.surface)
                .padding(horizontal = 15.dp, vertical = 13.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            val diff = levelToPct(sourceLevel) - levelToPct(twin.forecastLevel)
            SaveChip("↓ ${diff}%p 덜 붐빔")
            SaveChip("◷ 48분 대기 절약", brandTone = true)
            Spacer(Modifier.weight(1f))
            Box(
                Modifier.clip(androidx.compose.foundation.shape.RoundedCornerShape(999.dp))
                    .background(YeobaekTheme.colors.brand)
                    .clickableNoRipple(onApply)
                    .padding(horizontal = 16.dp, vertical = 10.dp),
            ) { Text("이 동선으로", color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 13.sp) }
        }
    }
}

@Composable
private fun AltColumn(
    modifier: Modifier,
    tag: String,
    tagColor: Color,
    bg: Color,
    name: String,
    cat: String,
    pct: Int,
    pctColor: Color,
    level: Int,
) {
    Column(modifier.background(bg).padding(horizontal = 14.dp, vertical = 16.dp)) {
        Text(tag.uppercase(), color = tagColor, fontSize = 10.5f.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
        Spacer(Modifier.height(8.dp))
        Text(name, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
        Text(cat, color = YeobaekTheme.colors.inkFaint, fontSize = 11.5f.sp, modifier = Modifier.padding(top = 3.dp))
        Row(Modifier.padding(top = 14.dp), verticalAlignment = Alignment.Bottom) {
            Text("$pct", color = pctColor, fontSize = 28.sp, fontWeight = FontWeight.W800)
            Text("%", color = YeobaekTheme.colors.inkFaint, fontSize = 12.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(bottom = 4.dp, start = 3.dp))
        }
        Spacer(Modifier.height(8.dp))
        DensityPill(level = level, compact = true)
    }
}

@Preview(showBackground = true, heightDp = 760)
@Composable
private fun DashboardPreview() {
    YeobaekTheme {
        Surface {
            DashboardScreen(match = SampleData.match, sourcePct = SampleData.heroPct, sourceLevel = SampleData.heroLevel)
        }
    }
}
