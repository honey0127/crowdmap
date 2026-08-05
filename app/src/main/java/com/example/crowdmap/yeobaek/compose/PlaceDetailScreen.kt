package com.example.crowdmap.yeobaek.compose

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.components.DensityPill
import com.example.crowdmap.yeobaek.compose.components.Eyebrow
import com.example.crowdmap.yeobaek.compose.components.SaveChip
import com.example.crowdmap.yeobaek.compose.components.hairline
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import com.example.crowdmap.yeobaek.data.OffpeakResponse
import com.example.crowdmap.yeobaek.data.PlaceResult
import com.example.crowdmap.yeobaek.data.Twin

/**
 * 화면 4 · 장소 상세 — 오프피크 시간대 + 미시적 분산 대안.
 * /api/v1/places/offpeak (시간대별 한적함) + /api/v1/places/disperse (도보권 더 한적한 곳).
 */
@Composable
fun PlaceDetailScreen(
    place: PlaceResult,
    offpeak: OffpeakResponse,
    hours: List<String>,
    alternatives: List<Twin>,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)
            .verticalScroll(rememberScrollState()),
    ) {
        DetailHero(place)
        Column(Modifier.padding(horizontal = 16.dp)) {
            Spacer(Modifier.height(18.dp))
            Eyebrow("Best time to visit")
            Text("언제 가면 한적할까요?", style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(top = 8.dp))
            Text(
                offpeak.area?.let { "$it 예보지점 기준 · 오늘" } ?: "오늘",
                color = YeobaekTheme.colors.inkFaint, fontSize = 12.sp, modifier = Modifier.padding(top = 2.dp),
            )
            Spacer(Modifier.height(14.dp))
            OffpeakChart(offpeak, hours)

            Spacer(Modifier.height(24.dp))
            Eyebrow("Walk a little quieter")
            Text("도보권, 더 한적한 대안", style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(top = 8.dp))
            Spacer(Modifier.height(12.dp))
            alternatives.forEach { t -> DisperseRow(t); Spacer(Modifier.height(8.dp)) }
            Spacer(Modifier.height(28.dp))
        }
    }
}

@Composable
private fun DetailHero(place: PlaceResult) {
    Box(
        Modifier.fillMaxWidth().height(210.dp)
            .background(Brush.linearGradient(listOf(YeobaekTheme.colors.brand, YeobaekTheme.colors.brandDeep))),
    ) {
        // 이미지 자리(실제 앱: 대표 사진). 여백 감성의 그라디언트 플레이스홀더.
        Column(Modifier.align(Alignment.BottomStart).padding(20.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                DensityPill(level = place.level, compact = true)
                Box(
                    Modifier.clip(RoundedCornerShape(999.dp)).background(Color(0x29FFFFFF)).padding(horizontal = 11.dp, vertical = 6.dp),
                ) { Text("한적함 ${place.quietScore ?: 0}", color = Color(0xFFEAF6F4), fontSize = 12.sp, fontWeight = FontWeight.Bold) }
            }
            Spacer(Modifier.height(10.dp))
            Text(place.title, color = Color(0xFFF3FAF9), fontSize = 28.sp, fontWeight = FontWeight.W800)
            Text(place.catLabel ?: "명소", color = Color(0xCCEAF6F4), fontSize = 13.sp, modifier = Modifier.padding(top = 2.dp))
        }
    }
}

/** 시간대별 한적함 막대 — 값이 높을수록(한적) 길고 초록, 낮을수록(붐빔) 짧고 붉음. */
@Composable
private fun OffpeakChart(offpeak: OffpeakResponse, hours: List<String>) {
    val bars = offpeak.timeline
    val bestIdx = bars.indices.maxByOrNull { bars[it].quietScore ?: 0 } ?: -1
    Column(
        Modifier.fillMaxWidth().clip(MaterialTheme.shapes.large).background(MaterialTheme.colorScheme.surface)
            .hairline(MaterialTheme.shapes.large).padding(16.dp),
    ) {
        Row(
            Modifier.fillMaxWidth().height(120.dp),
            verticalAlignment = Alignment.Bottom,
            horizontalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            bars.forEachIndexed { i, hour ->
                val q = (hour.quietScore ?: 0).coerceIn(0, 100)
                val d = YeobaekTheme.colors.density(hour.level)
                Column(Modifier.weight(1f), horizontalAlignment = Alignment.CenterHorizontally) {
                    Box(
                        Modifier.fillMaxWidth().height((q * 1.0f).dp.coerceAtLeast(6.dp))
                            .clip(RoundedCornerShape(topStart = 5.dp, topEnd = 5.dp))
                            .background(if (i == bestIdx) d.fg else d.fg.copy(alpha = 0.45f)),
                    )
                    Text(hours.getOrElse(i) { "" }, fontSize = 9.5f.sp, color = YeobaekTheme.colors.inkFaint, modifier = Modifier.padding(top = 6.dp))
                }
            }
        }
        Spacer(Modifier.height(12.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            SaveChip("추천 ${hours.getOrElse(bestIdx) { "--" }}시 · 한적함 ${bars.getOrNull(bestIdx)?.quietScore ?: 0}", brandTone = true)
            Spacer(Modifier.weight(1f))
            Text("피크 12–13시", color = YeobaekTheme.colors.density(3).ink, fontSize = 11.5f.sp, fontWeight = FontWeight.SemiBold)
        }
    }
}

@Composable
private fun DisperseRow(t: Twin) {
    Row(
        Modifier.fillMaxWidth().clip(MaterialTheme.shapes.medium).background(MaterialTheme.colorScheme.surface)
            .hairline(MaterialTheme.shapes.medium).padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(Modifier.size(40.dp).clip(CircleShape).background(YeobaekTheme.colors.brandTint), contentAlignment = Alignment.Center) {
            Text("旅", color = YeobaekTheme.colors.brand, fontWeight = FontWeight.Bold, fontSize = 15.sp)
        }
        Spacer(Modifier.width(12.dp))
        Column(Modifier.weight(1f)) {
            Text(t.title, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
            Text("유사도 ${"%.2f".format(t.similarity)} · ${"%.1f".format(t.distKm)}km", color = YeobaekTheme.colors.inkFaint, fontSize = 11.5f.sp, modifier = Modifier.padding(top = 2.dp))
        }
        DensityPill(level = t.forecastLevel, compact = true)
    }
}

@Preview(showBackground = true, heightDp = 900)
@Composable
private fun PlaceDetailPreview() {
    YeobaekTheme {
        Surface {
            PlaceDetailScreen(
                place = SampleData.detailPlace,
                offpeak = SampleData.offpeak,
                hours = SampleData.offpeakHours,
                alternatives = SampleData.match.twins,
            )
        }
    }
}
