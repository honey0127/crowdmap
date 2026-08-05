package com.example.crowdmap.yeobaek.compose.theme

import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color

/**
 * 여백(旅白) 디자인 토큰 — "차분한 지성" 슬레이트 틸.
 *
 * 참고: 기존 res/values/yeobaek.xml 은 View/XML 화면용 그린(#0FB86B) 승인 시안이다.
 * 이 파일은 새 디자인 브리프(Deep Slate Teal)를 따르는 Compose 전용 팔레트로, 두 시스템은
 * 공존한다. 혼잡 밀도는 accent 와 분리된 '시맨틱 컬러'(세이지/앰버/뮤트 크림슨)로, 채도를
 * 낮춰 "경고하되 소리치지 않게" 한다.
 */
internal object Palette {
    // ── Brand / Neutral (Light) ──
    val BrandTeal = Color(0xFF17595E)
    val BrandDeep = Color(0xFF0F3E42)
    val BrandTint = Color(0xFFE7F1F0)
    val BrandInk = Color(0xFF0C3134)
    val ChalkBg = Color(0xFFF9FAFB)
    val SurfaceLight = Color(0xFFFFFFFF)
    val InkCharcoal = Color(0xFF111827)
    val InkSlate = Color(0xFF6B7280)
    val InkFaint = Color(0xFF8A929C)
    val HairlineLight = Color(0x1A133C40) // 틸 10%
    val HairlineFaint = Color(0x0E133C40) // 틸 5.5%

    // ── Brand / Neutral (Dark) — 틸 바이어스 다크 ──
    val BrandTealDark = Color(0xFF57B7B2)
    val BrandDeepDark = Color(0xFF2C6E6C)
    val BrandTintDark = Color(0x2257B7B2)
    val BrandInkDark = Color(0xFFBFE6E2)
    val BgDark = Color(0xFF0B1516)
    val SurfaceDark = Color(0xFF132424)
    val SurfaceDark2 = Color(0xFF172C2C)
    val InkDark = Color(0xFFEAF1F0)
    val InkSlateDark = Color(0xFF9FB0AF)
    val InkFaintDark = Color(0xFF748584)
    val HairlineDark = Color(0x1AFFFFFF)
    val HairlineFaintDark = Color(0x0FFFFFFF)

    // ── Density semantics (forecast_level 1..4), Light ──
    val Den1 = Color(0xFF3F8266); val Den1Bg = Color(0xFFE6F0EA); val Den1Ink = Color(0xFF245640)
    val Den2 = Color(0xFFB4863F); val Den2Bg = Color(0xFFF6EEDC); val Den2Ink = Color(0xFF7C5A1E)
    val Den3 = Color(0xFFAF585A); val Den3Bg = Color(0xFFF5E4E4); val Den3Ink = Color(0xFF7C3739)
    val Den4 = Color(0xFF93454A); val Den4Bg = Color(0xFFEFD9DA); val Den4Ink = Color(0xFF6C2C31)

    // ── Density semantics (Dark) ──
    val Den1D = Color(0xFF67B592); val Den1BgD = Color(0x2667B592); val Den1InkD = Color(0xFF8FD3B2)
    val Den2D = Color(0xFFD3A85E); val Den2BgD = Color(0x26D3A85E); val Den2InkD = Color(0xFFE7C489)
    val Den3D = Color(0xFFCF7A7C); val Den3BgD = Color(0x29CF7A7C); val Den3InkD = Color(0xFFE8A3A4)
    val Den4D = Color(0xFFC56B70); val Den4BgD = Color(0x2EC56B70); val Den4InkD = Color(0xFFE49B9F)
}

/** 혼잡 레벨 한 단계의 삼색 세트(전경/배경/텍스트). */
data class DensityColor(val fg: Color, val bg: Color, val ink: Color)

/**
 * Material3 ColorScheme 이 담지 못하는 여백 전용 확장 토큰.
 * [YeobaekTheme] 이 CompositionLocal 로 제공하며, `YeobaekTheme.colors` 로 접근한다.
 */
data class YeobaekColors(
    val brand: Color,
    val brandDeep: Color,
    val brandTint: Color,
    val brandInk: Color,
    val hairline: Color,
    val hairlineFaint: Color,
    val inkSlate: Color,
    val inkFaint: Color,
    val panel: Color,
    val density: List<DensityColor>, // index 0..3 ↔ level 1..4
    val heroBrush: Brush,
    val isDark: Boolean,
) {
    /** forecast_level(1~4). 범위 밖/미매핑은 중립 Lv.2 로 폴백(엔진 계약과 동일). */
    fun density(level: Int?): DensityColor =
        density[((level ?: 2).coerceIn(1, 4)) - 1]
}

internal fun lightYeobaekColors(): YeobaekColors = YeobaekColors(
    brand = Palette.BrandTeal,
    brandDeep = Palette.BrandDeep,
    brandTint = Palette.BrandTint,
    brandInk = Palette.BrandInk,
    hairline = Palette.HairlineLight,
    hairlineFaint = Palette.HairlineFaint,
    inkSlate = Palette.InkSlate,
    inkFaint = Palette.InkFaint,
    panel = Palette.ChalkBg,
    density = listOf(
        DensityColor(Palette.Den1, Palette.Den1Bg, Palette.Den1Ink),
        DensityColor(Palette.Den2, Palette.Den2Bg, Palette.Den2Ink),
        DensityColor(Palette.Den3, Palette.Den3Bg, Palette.Den3Ink),
        DensityColor(Palette.Den4, Palette.Den4Bg, Palette.Den4Ink),
    ),
    heroBrush = Brush.linearGradient(listOf(Palette.BrandTeal, Palette.BrandDeep)),
    isDark = false,
)

internal fun darkYeobaekColors(): YeobaekColors = YeobaekColors(
    brand = Palette.BrandTealDark,
    brandDeep = Palette.BrandDeepDark,
    brandTint = Palette.BrandTintDark,
    brandInk = Palette.BrandInkDark,
    hairline = Palette.HairlineDark,
    hairlineFaint = Palette.HairlineFaintDark,
    inkSlate = Palette.InkSlateDark,
    inkFaint = Palette.InkFaintDark,
    panel = Palette.BgDark,
    density = listOf(
        DensityColor(Palette.Den1D, Palette.Den1BgD, Palette.Den1InkD),
        DensityColor(Palette.Den2D, Palette.Den2BgD, Palette.Den2InkD),
        DensityColor(Palette.Den3D, Palette.Den3BgD, Palette.Den3InkD),
        DensityColor(Palette.Den4D, Palette.Den4BgD, Palette.Den4InkD),
    ),
    heroBrush = Brush.linearGradient(listOf(Palette.BrandDeepDark, Palette.BrandTealDark)),
    isDark = true,
)

/** 혼잡 레벨 라벨(엔진 Congestion.label 과 톤 일치). */
fun densityLabel(level: Int?): String = when (level) {
    1 -> "여유로운"
    2 -> "보통"
    3 -> "혼잡한"
    4 -> "매우 혼잡"
    else -> "보통"
}

/** 짧은 배지 라벨. */
fun densityShort(level: Int?): String = when (level) {
    1 -> "여유"; 2 -> "보통"; 3 -> "혼잡"; 4 -> "매우혼잡"; else -> "보통"
}
