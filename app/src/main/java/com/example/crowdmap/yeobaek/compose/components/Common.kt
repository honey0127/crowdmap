package com.example.crowdmap.yeobaek.compose.components

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.theme.PillShape
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme
import com.example.crowdmap.yeobaek.compose.theme.densityShort

/** 그림자 대신 얇은 1dp 틸-바이어스 보더 = 여백 감성. */
@Composable
fun Modifier.hairline(shape: Shape, faint: Boolean = false): Modifier {
    val c = if (faint) YeobaekTheme.colors.hairlineFaint else YeobaekTheme.colors.hairline
    return this.border(1.dp, c, shape)
}

/** 리플 없는 가벼운 클릭(라벨/텍스트 액션용). */
@Composable
fun Modifier.clickableNoRipple(onClick: () -> Unit): Modifier {
    val interaction = remember { MutableInteractionSource() }
    return this.clickable(interactionSource = interaction, indication = null, onClick = onClick)
}

/** 혼잡 밀도 배지 — 색+라벨로 상태를 한눈에. */
@Composable
fun DensityPill(
    level: Int?,
    modifier: Modifier = Modifier,
    showLevel: Boolean = true,
    compact: Boolean = false,
) {
    val d = YeobaekTheme.colors.density(level)
    Row(
        modifier
            .clip(PillShape)
            .background(d.bg)
            .padding(horizontal = if (compact) 9.dp else 12.dp, vertical = if (compact) 4.dp else 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(7.dp),
    ) {
        Box(Modifier.size(8.dp).clip(CircleShape).background(d.fg))
        Text(
            text = if (showLevel) "Lv.${(level ?: 2).coerceIn(1, 4)} ${densityShort(level)}" else densityShort(level),
            color = d.ink,
            fontSize = if (compact) 11.sp else 13.sp,
            fontWeight = FontWeight.SemiBold,
        )
    }
}

/**
 * 세그먼트 혼잡 미터 — 하루 [segments]칸 중 '꺼진 칸'이 여백(여유). [ratio] 0f..1f 채워짐.
 * onColor/offColor 로 글래스 히어로(밝은 배경)와 카드(전경) 모두에서 재사용.
 */
@Composable
fun DensityMeter(
    ratio: Float,
    modifier: Modifier = Modifier,
    onColor: Color = Color(0xFFEAF6F4),
    offColor: Color = Color(0x2EFFFFFF),
    segments: Int = 12,
) {
    val anim by animateFloatAsState(targetValue = ratio.coerceIn(0f, 1f), label = "meter")
    val onCount = (anim * segments).toInt()
    Row(
        modifier
            .fillMaxWidth()
            .height(9.dp)
            .clip(PillShape)
            .background(offColor)
            .padding(horizontal = 1.dp),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        repeat(segments) { i ->
            Box(
                Modifier
                    .weight(1f)
                    .height(7.dp)
                    .clip(RoundedCornerShape(2.dp))
                    .background(if (i < onCount) onColor else Color.Transparent)
            )
        }
    }
}

/** 얇은 수평 밀도 바 (리스트/상세용). */
@Composable
fun DensityBar(level: Int?, ratio: Float, modifier: Modifier = Modifier) {
    val d = YeobaekTheme.colors.density(level)
    val anim by animateFloatAsState(ratio.coerceIn(0f, 1f), label = "bar")
    Box(
        modifier
            .height(8.dp)
            .clip(PillShape)
            .background(YeobaekTheme.colors.hairlineFaint)
    ) {
        Box(
            Modifier
                .fillMaxWidth(anim)
                .height(8.dp)
                .clip(PillShape)
                .background(d.fg)
        )
    }
}

/** 절약 지표 pill — 시간/혼잡 절감 요약. */
@Composable
fun SaveChip(text: String, brandTone: Boolean = false, modifier: Modifier = Modifier) {
    val bg = if (brandTone) YeobaekTheme.colors.brandTint else YeobaekTheme.colors.density(1).bg
    val fg = if (brandTone) YeobaekTheme.colors.brandInk else YeobaekTheme.colors.density(1).ink
    Row(
        modifier.clip(PillShape).background(bg).padding(horizontal = 11.dp, vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(text, color = fg, fontSize = 12.5f.sp, fontWeight = FontWeight.Bold)
    }
}

/** 섹션 아이브로우 — 대문자 트래킹 + 짧은 룰. */
@Composable
fun Eyebrow(text: String, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        Box(Modifier.width(18.dp).height(1.5.dp).background(YeobaekTheme.colors.brand))
        Text(
            text.uppercase(),
            color = YeobaekTheme.colors.brand,
            fontSize = 12.sp,
            fontWeight = FontWeight.Bold,
            letterSpacing = 1.8.sp,
        )
    }
}

/** 리스트 섹션 헤더 — 타이틀 + 우측 텍스트 액션. */
@Composable
fun SectionHeader(title: String, action: String? = null, onAction: (() -> Unit)? = null) {
    Row(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.weight(1f))
        if (action != null) {
            Text(
                action,
                color = YeobaekTheme.colors.brand,
                fontSize = 12.5f.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = if (onAction != null) Modifier.clickableNoRipple(onAction) else Modifier,
            )
        }
    }
}
