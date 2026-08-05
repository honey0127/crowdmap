package com.example.crowdmap.yeobaek.compose.theme

import androidx.compose.material3.Typography
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.sp

/**
 * 타이포그래피 — Pretendard / SF Pro 계열의 크리스프한 위계.
 *
 * 폰트: 실제 앱에선 Pretendard 를 res/font 에 번들해 아래 [YeobaekFont] 를 교체하세요.
 * 미번들 시 시스템 산세리프로 폴백(디바이스 기본이 Pretendard/SF Pro 계열이면 자연스럽게 렌더).
 *
 *   val YeobaekFont = FontFamily(
 *       Font(R.font.pretendard_regular, FontWeight.Normal),
 *       Font(R.font.pretendard_medium,  FontWeight.Medium),
 *       Font(R.font.pretendard_semibold,FontWeight.SemiBold),
 *       Font(R.font.pretendard_bold,    FontWeight.Bold),
 *   )
 */
val YeobaekFont: FontFamily = FontFamily.Default

val YeobaekTypography = Typography(
    // Display — 페이지 헤더 "여백의 발견"
    displaySmall = TextStyle(
        fontFamily = YeobaekFont, fontSize = 34.sp, lineHeight = 38.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W800, letterSpacing = (-1.3).sp,
    ),
    headlineSmall = TextStyle(
        fontFamily = YeobaekFont, fontSize = 24.sp, lineHeight = 28.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W700, letterSpacing = (-0.8).sp,
    ),
    // Title — 카드/스톱 이름 "북촌한옥마을"
    titleLarge = TextStyle(
        fontFamily = YeobaekFont, fontSize = 20.sp, lineHeight = 24.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W700, letterSpacing = (-0.6).sp,
    ),
    titleMedium = TextStyle(
        fontFamily = YeobaekFont, fontSize = 16.sp, lineHeight = 20.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W700, letterSpacing = (-0.4).sp,
    ),
    // Body
    bodyLarge = TextStyle(
        fontFamily = YeobaekFont, fontSize = 16.sp, lineHeight = 24.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W400, letterSpacing = (-0.2).sp,
    ),
    bodyMedium = TextStyle(
        fontFamily = YeobaekFont, fontSize = 14.sp, lineHeight = 20.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W400, letterSpacing = (-0.1).sp,
    ),
    bodySmall = TextStyle(
        fontFamily = YeobaekFont, fontSize = 12.5f.sp, lineHeight = 17.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W400,
    ),
    // Label / Badge — 대문자 트래킹 "LOW DENSITY"
    labelLarge = TextStyle(
        fontFamily = YeobaekFont, fontSize = 13.sp, lineHeight = 16.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W600, letterSpacing = (-0.1).sp,
    ),
    labelMedium = TextStyle(
        fontFamily = YeobaekFont, fontSize = 12.sp, lineHeight = 15.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W600,
    ),
    labelSmall = TextStyle(
        fontFamily = YeobaekFont, fontSize = 11.sp, lineHeight = 14.sp,
        fontWeight = androidx.compose.ui.text.font.FontWeight.W700, letterSpacing = 1.5.sp,
        textAlign = TextAlign.Start,
    ),
)
