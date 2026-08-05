package com.example.crowdmap.yeobaek.compose

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.components.DensityPill
import com.example.crowdmap.yeobaek.compose.components.Eyebrow
import com.example.crowdmap.yeobaek.compose.components.hairline
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import com.example.crowdmap.yeobaek.compose.theme.densityShort
import com.example.crowdmap.yeobaek.data.PlaceResult

/**
 * 화면 3 · 혼잡 히트맵 — /api/v1/places/heatmap (level·quietScore).
 *
 * 참고: 실제 앱은 com.google.maps.android:maps-compose 의 GoogleMap 위에 마커/히트 레이어를
 * 올린다(마커 색조 = Congestion.hue). 여기서는 지도 SDK 의존 없이 스키매틱 캔버스로 배치를
 * 검증한다 — 좌표를 화면 비율로 정규화해 밀도색 핀을 찍는다.
 */
@Composable
fun MapHeatmapScreen(
    places: List<PlaceResult>,
    modifier: Modifier = Modifier,
) {
    val quiet = places.sortedByDescending { it.quietScore ?: 0 }
    Column(
        modifier.fillMaxSize().background(MaterialTheme.colorScheme.background)
            .verticalScroll(rememberScrollState()).padding(horizontal = 16.dp),
    ) {
        Column(Modifier.padding(top = 16.dp, bottom = 12.dp)) {
            Eyebrow("Congestion heatmap")
            Text("이 지역의 여백", style = MaterialTheme.typography.headlineSmall, modifier = Modifier.padding(top = 8.dp))
            Text("붉을수록 붐비고, 초록일수록 한적합니다.", color = YeobaekTheme.colors.inkSlate, fontSize = 13.sp, modifier = Modifier.padding(top = 4.dp))
        }

        HeatCanvas(places)
        Spacer(Modifier.height(12.dp))
        HeatLegend()

        Spacer(Modifier.height(20.dp))
        Text("가장 한적한 순", style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(10.dp))
        quiet.forEach { p -> QuietRow(p); Spacer(Modifier.height(8.dp)) }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun HeatCanvas(places: List<PlaceResult>) {
    val lats = places.mapNotNull { it.lat }
    val lngs = places.mapNotNull { it.lng }
    val minLat = lats.minOrNull() ?: 0.0; val maxLat = lats.maxOrNull() ?: 1.0
    val minLng = lngs.minOrNull() ?: 0.0; val maxLng = lngs.maxOrNull() ?: 1.0
    val padFrac = 0.12f

    BoxWithConstraints(
        Modifier.fillMaxWidth().height(240.dp).clip(MaterialTheme.shapes.large)
            .background(YeobaekTheme.colors.panel).hairline(MaterialTheme.shapes.large),
    ) {
        val w = maxWidth; val h = maxHeight
        // 은은한 격자 느낌의 중립 배경 위 핀
        places.forEach { p ->
            val lat = p.lat ?: return@forEach
            val lng = p.lng ?: return@forEach
            val fx = ((lng - minLng) / (maxLng - minLng + 1e-9)).toFloat().coerceIn(0f, 1f)
            val fy = 1f - ((lat - minLat) / (maxLat - minLat + 1e-9)).toFloat().coerceIn(0f, 1f)
            val x = w * (padFrac + fx * (1 - 2 * padFrac)).toFloat()
            val y = h * (padFrac + fy * (1 - 2 * padFrac)).toFloat()
            HeatPin(level = p.level, label = p.title, xOffset = x, yOffset = y)
        }
    }
}

@Composable
private fun HeatPin(level: Int?, label: String, xOffset: androidx.compose.ui.unit.Dp, yOffset: androidx.compose.ui.unit.Dp) {
    val d = YeobaekTheme.colors.density(level)
    Column(
        Modifier.offset(x = xOffset - 26.dp, y = yOffset - 22.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        // 히트 글로우 + 핀
        Box(Modifier.size(44.dp).clip(CircleShape).background(d.fg.copy(alpha = 0.16f)), contentAlignment = Alignment.Center) {
            Box(Modifier.size(18.dp).clip(CircleShape).background(d.fg).hairline(CircleShape))
        }
        Text(
            label, fontSize = 9.5f.sp, fontWeight = FontWeight.SemiBold, color = MaterialTheme.colorScheme.onSurface,
            modifier = Modifier.padding(top = 2.dp).clip(RoundedCornerShape(6.dp)).background(MaterialTheme.colorScheme.surface.copy(alpha = 0.85f)).padding(horizontal = 4.dp),
        )
    }
}

@Composable
private fun HeatLegend() {
    Row(
        Modifier.fillMaxWidth().clip(MaterialTheme.shapes.medium).background(MaterialTheme.colorScheme.surface)
            .hairline(MaterialTheme.shapes.medium).padding(horizontal = 14.dp, vertical = 11.dp),
        horizontalArrangement = Arrangement.spacedBy(14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        (1..4).forEach { lv ->
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Box(Modifier.size(10.dp).clip(CircleShape).background(YeobaekTheme.colors.density(lv).fg))
                Text(densityShort(lv), fontSize = 11.5f.sp, color = YeobaekTheme.colors.inkSlate, fontWeight = FontWeight.Medium)
            }
        }
    }
}

@Composable
private fun QuietRow(p: PlaceResult) {
    Row(
        Modifier.fillMaxWidth().clip(MaterialTheme.shapes.medium).background(MaterialTheme.colorScheme.surface)
            .hairline(MaterialTheme.shapes.medium).padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(p.title, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
            Text(p.catLabel ?: "명소", color = YeobaekTheme.colors.inkFaint, fontSize = 11.5f.sp, modifier = Modifier.padding(top = 2.dp))
        }
        Column(horizontalAlignment = Alignment.End) {
            Text("한적함 ${p.quietScore ?: 0}", color = YeobaekTheme.colors.brand, fontSize = 12.sp, fontWeight = FontWeight.Bold)
            Spacer(Modifier.height(6.dp))
            DensityPill(level = p.level, compact = true)
        }
    }
}

@Preview(showBackground = true, heightDp = 900)
@Composable
private fun MapHeatmapPreview() {
    YeobaekTheme { Surface { MapHeatmapScreen(places = SampleData.heatmap) } }
}
