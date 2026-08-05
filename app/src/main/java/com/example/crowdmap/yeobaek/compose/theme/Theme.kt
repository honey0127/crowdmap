package com.example.crowdmap.yeobaek.compose.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf

private val LightScheme = lightColorScheme(
    primary = Palette.BrandTeal,
    onPrimary = Palette.SurfaceLight,
    primaryContainer = Palette.BrandTint,
    onPrimaryContainer = Palette.BrandInk,
    secondary = Palette.BrandDeep,
    background = Palette.ChalkBg,
    onBackground = Palette.InkCharcoal,
    surface = Palette.SurfaceLight,
    onSurface = Palette.InkCharcoal,
    surfaceVariant = Palette.ChalkBg,
    onSurfaceVariant = Palette.InkSlate,
    outline = Palette.HairlineLight,
    outlineVariant = Palette.HairlineFaint,
    error = Palette.Den4,
)

private val DarkScheme = darkColorScheme(
    primary = Palette.BrandTealDark,
    onPrimary = Palette.BgDark,
    primaryContainer = Palette.BrandTintDark,
    onPrimaryContainer = Palette.BrandInkDark,
    secondary = Palette.BrandDeepDark,
    background = Palette.BgDark,
    onBackground = Palette.InkDark,
    surface = Palette.SurfaceDark,
    onSurface = Palette.InkDark,
    surfaceVariant = Palette.SurfaceDark2,
    onSurfaceVariant = Palette.InkSlateDark,
    outline = Palette.HairlineDark,
    outlineVariant = Palette.HairlineFaintDark,
    error = Palette.Den4D,
)

private val LocalYeobaekColors = staticCompositionLocalOf { lightYeobaekColors() }

@Composable
fun YeobaekTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    val extended = if (darkTheme) darkYeobaekColors() else lightYeobaekColors()
    CompositionLocalProvider(LocalYeobaekColors provides extended) {
        MaterialTheme(
            colorScheme = if (darkTheme) DarkScheme else LightScheme,
            typography = YeobaekTypography,
            shapes = YeobaekShapes,
            content = content,
        )
    }
}

/** 여백 확장 토큰 접근자 — `YeobaekTheme.colors.density(level)` 등. */
object YeobaekTheme {
    val colors: YeobaekColors
        @Composable @ReadOnlyComposable
        get() = LocalYeobaekColors.current
}
