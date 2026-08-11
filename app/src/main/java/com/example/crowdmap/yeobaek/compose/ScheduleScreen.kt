package com.example.crowdmap.yeobaek.compose

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
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.components.DensityPill
import com.example.crowdmap.yeobaek.compose.components.SectionHeader
import com.example.crowdmap.yeobaek.compose.components.hairline
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import com.example.crowdmap.yeobaek.data.PlanStop
import com.example.crowdmap.yeobaek.data.ScheduleResponse

/**
 * 화면 2 · 여백 스케줄러 — 스마트 스케줄 타임라인.
 * 상단 Density Buffer(절약 서사) + 도착·체류·혼잡 노드 타임라인 + 이동 간격.
 */
@Composable
fun ScheduleScreen(
    schedule: ScheduleResponse,
    dwellMin: List<Int>,
    transit: List<String>,
    modifier: Modifier = Modifier,
    isOffline: Boolean = false,
) {
    Column(
        modifier
            .fillMaxSize()
            .background(MaterialTheme.colorScheme.background)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp),
    ) {
        if (isOffline) OfflineBanner()

        Row(Modifier.fillMaxWidth().padding(top = 16.dp, bottom = 14.dp), verticalAlignment = Alignment.Top) {
            Column(Modifier.weight(1f)) {
                Text("10월 11일 · 최적화 완료", color = YeobaekTheme.colors.inkFaint, fontSize = 12.sp, fontWeight = FontWeight.Medium)
                Text("오늘의 여백", style = MaterialTheme.typography.titleLarge, modifier = Modifier.padding(top = 3.dp))
            }
            schedule.yeobaekIndex?.let { YeobaekIndexBadge(it) }
        }

        DensityBuffer(savedPct = schedule.savedCongestionPct, stopsChanged = schedule.ordered.count { it.substitutedFrom != null })

        Spacer(Modifier.height(18.dp))
        SectionHeader(title = "최적화된 동선", action = "순서 편집")
        Spacer(Modifier.height(8.dp))

        schedule.ordered.forEachIndexed { i, stop ->
            TimelineStop(
                stop = stop,
                dwellMin = dwellMin.getOrNull(i),
                isLast = i == schedule.ordered.lastIndex,
            )
            if (i < schedule.ordered.lastIndex) {
                TransitRow(label = transit.getOrNull(i) ?: "이동")
            }
        }
        Spacer(Modifier.height(24.dp))
    }
}

/** 서버 연결 실패 시 로컬에 저장된 마지막 계획을 보여주고 있음을 알리는 배너. */
@Composable
private fun OfflineBanner() {
    Row(
        Modifier.fillMaxWidth().padding(top = 12.dp).clip(MaterialTheme.shapes.medium)
            .background(YeobaekTheme.colors.density(3).bg)
            .padding(horizontal = 14.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            "오프라인 · 서버에 연결하지 못해 마지막으로 저장된 계획을 보여드려요",
            color = YeobaekTheme.colors.density(3).ink, fontSize = 12.sp, fontWeight = FontWeight.Medium,
        )
    }
}

/** 여백 지수 배지 — 혼잡·유사도·이동효율을 합산한 이 계획의 단일 점수(0~100). 브랜드 지표. */
@Composable
private fun YeobaekIndexBadge(index: Int) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier.size(56.dp).clip(CircleShape)
                .background(Brush.linearGradient(listOf(YeobaekTheme.colors.brand, YeobaekTheme.colors.brandDeep))),
            contentAlignment = Alignment.Center,
        ) {
            Text("$index", color = Color(0xFFF3FAF9), fontSize = 20.sp, fontWeight = FontWeight.W800)
        }
        Text("여백지수", color = YeobaekTheme.colors.inkFaint, fontSize = 10.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(top = 4.dp))
    }
}

/** Real-time Density Buffer — 재배치로 아낀 시간/스트레스 서사. */
@Composable
private fun DensityBuffer(savedPct: Int, stopsChanged: Int) {
    // savedPct(혼잡 절감)를 시간 절약 서사로 환산(디자인 근사: 1%p ≈ 1.7분).
    val savedMin = (savedPct * 1.7f).toInt()
    val h = savedMin / 60
    val m = savedMin % 60
    Box(
        Modifier
            .fillMaxWidth()
            .clip(MaterialTheme.shapes.large)
            .background(Brush.linearGradient(listOf(YeobaekTheme.colors.brandDeep, YeobaekTheme.colors.brand)))
            .padding(horizontal = 20.dp, vertical = 18.dp),
    ) {
        Column {
            Text("REAL-TIME DENSITY BUFFER", color = Color(0xD1EAF6F4), fontSize = 11.5f.sp, fontWeight = FontWeight.SemiBold, letterSpacing = 1.6.sp)
            Spacer(Modifier.height(6.dp))
            Row(verticalAlignment = Alignment.Bottom) {
                Text(
                    buildString { if (h > 0) append("${h}시간 "); append("${m}분") },
                    color = Color(0xFFEAF6F4), fontSize = 32.sp, fontWeight = FontWeight.W800,
                )
                Text(" 절약", color = Color(0xD1EAF6F4), fontSize = 15.sp, fontWeight = FontWeight.SemiBold, modifier = Modifier.padding(bottom = 3.dp))
            }
            Text(
                "${stopsChanged}곳 재배치 · 평균 혼잡 Lv.3.1 → Lv.1.6",
                color = Color(0xD1EAF6F4), fontSize = 12.5f.sp, fontWeight = FontWeight.Normal, modifier = Modifier.padding(top = 4.dp),
            )
            Spacer(Modifier.height(14.dp))
            Box(Modifier.fillMaxWidth().height(7.dp).clip(RoundedCornerShape(999.dp)).background(Color(0x33FFFFFF))) {
                Box(Modifier.fillMaxWidth(0.64f).height(7.dp).clip(RoundedCornerShape(999.dp)).background(Color(0xFF8FE3BE)))
            }
        }
    }
}

@Composable
private fun TimelineStop(stop: PlanStop, dwellMin: Int?, isLast: Boolean) {
    val nodeColor = YeobaekTheme.colors.density(stop.forecastLevel).fg
    val substituted = stop.substitutedFrom != null
    Row(Modifier.fillMaxWidth()) {
        // 시각 축
        Column(Modifier.width(56.dp).padding(top = 15.dp), horizontalAlignment = Alignment.End) {
            Text(stop.arrival, fontSize = 14.sp, fontWeight = FontWeight.Bold, color = MaterialTheme.colorScheme.onSurface)
            if (dwellMin != null) Text("${dwellMin}분 체류", fontSize = 10.5f.sp, color = YeobaekTheme.colors.inkFaint, textAlign = TextAlign.End)
        }
        Spacer(Modifier.width(12.dp))
        // 레일
        Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.width(16.dp)) {
            Box(
                Modifier.padding(top = 17.dp).size(15.dp).clip(CircleShape)
                    .background(MaterialTheme.colorScheme.surface)
                    .hairline(CircleShape),
                contentAlignment = Alignment.Center,
            ) { Box(Modifier.size(9.dp).clip(CircleShape).background(nodeColor)) }
            if (!isLast) Box(Modifier.width(2.dp).height(64.dp).background(YeobaekTheme.colors.hairline))
        }
        Spacer(Modifier.width(12.dp))
        // 스톱 카드
        Column(
            Modifier.weight(1f)
                .padding(bottom = 12.dp)
                .clip(MaterialTheme.shapes.medium)
                .background(if (substituted) YeobaekTheme.colors.density(1).bg else MaterialTheme.colorScheme.surface)
                .hairline(MaterialTheme.shapes.medium, faint = !substituted)
                .padding(14.dp),
        ) {
            Row(verticalAlignment = Alignment.Top) {
                Column(Modifier.weight(1f)) {
                    Text(stop.title, style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onSurface)
                    if (substituted) {
                        Text("⇄ 대체됨 · 감성 쌍둥이", color = YeobaekTheme.colors.density(1).ink, fontSize = 11.sp, fontWeight = FontWeight.Bold, modifier = Modifier.padding(top = 4.dp))
                    } else {
                        Text("혼잡 예측 반영 · 도착 최적", color = YeobaekTheme.colors.inkFaint, fontSize = 11.5f.sp, modifier = Modifier.padding(top = 2.dp))
                    }
                }
                DensityPill(level = stop.forecastLevel, compact = true, showLevel = false)
            }
            Row(Modifier.padding(top = 12.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                MetaTag("◷ ${dwellMin ?: 60}분")
                MetaTag("Lv.${stop.forecastLevel}")
                if (substituted) MetaTag("↓ 혼잡 절감")
            }
        }
    }
}

@Composable
private fun MetaTag(text: String) {
    Box(
        Modifier.clip(RoundedCornerShape(999.dp)).background(YeobaekTheme.colors.panel)
            .hairline(RoundedCornerShape(999.dp), faint = true)
            .padding(horizontal = 9.dp, vertical = 5.dp),
    ) { Text(text, fontSize = 11.5f.sp, color = YeobaekTheme.colors.inkSlate, fontWeight = FontWeight.Medium) }
}

@Composable
private fun TransitRow(label: String) {
    // 카드 열 시작 위치에 맞춤: 시각축 56 + gap 12 + 레일 16 + gap 12 = 96dp
    Row(
        Modifier.fillMaxWidth().padding(start = 96.dp, bottom = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Box(Modifier.width(20.dp).height(1.dp).background(YeobaekTheme.colors.hairline))
        Text(label, fontSize = 11.5f.sp, color = YeobaekTheme.colors.inkFaint, fontWeight = FontWeight.Medium)
        Box(Modifier.weight(1f).height(1.dp).background(YeobaekTheme.colors.hairline))
    }
}

@Preview(showBackground = true, heightDp = 860)
@Composable
private fun SchedulePreview() {
    YeobaekTheme {
        Surface {
            ScheduleScreen(schedule = SampleData.schedule, dwellMin = SampleData.dwellMin, transit = SampleData.transit)
        }
    }
}
