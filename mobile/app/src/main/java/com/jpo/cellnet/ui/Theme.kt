package com.jpo.cellnet.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Forest = Color(0xFF0B3D2E)
private val Mint = Color(0xFF2F9E6B)
private val Sand = Color(0xFFF7F3EA)
private val Ink = Color(0xFF10231C)
private val Night = Color(0xFF0E1512)
private val NightSurface = Color(0xFF17231E)

private val LightColors = lightColorScheme(
    primary = Forest,
    onPrimary = Color.White,
    primaryContainer = Color(0xFFCDEBDC),
    onPrimaryContainer = Forest,
    secondary = Mint,
    onSecondary = Color.White,
    background = Sand,
    onBackground = Ink,
    surface = Color.White,
    onSurface = Ink,
    surfaceVariant = Color(0xFFECE5D8),
    onSurfaceVariant = Color(0xFF4A4A42)
)

private val DarkColors = darkColorScheme(
    primary = Color(0xFF7BE0B0),
    onPrimary = Color(0xFF06301F),
    primaryContainer = Color(0xFF1E4A38),
    onPrimaryContainer = Color(0xFFCDEBDC),
    secondary = Mint,
    onSecondary = Color.White,
    background = Night,
    onBackground = Color(0xFFE6EFE9),
    surface = NightSurface,
    onSurface = Color(0xFFE6EFE9),
    surfaceVariant = Color(0xFF2A3731),
    onSurfaceVariant = Color(0xFFC3CEC7)
)

@Composable
fun CellNetTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColors else LightColors,
        content = content
    )
}
