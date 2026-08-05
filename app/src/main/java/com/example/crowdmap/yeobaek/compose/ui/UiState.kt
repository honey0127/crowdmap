package com.example.crowdmap.yeobaek.compose.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.crowdmap.yeobaek.compose.components.clickableNoRipple
import com.example.crowdmap.yeobaek.compose.theme.YeobaekTheme

/** 화면 데이터 로드 상태. */
sealed interface UiState<out T> {
    data object Loading : UiState<Nothing>
    data class Success<T>(val data: T) : UiState<T>
    data class Error(val message: String) : UiState<Nothing>
}

/**
 * Loading/Error 를 공통 처리하고, Success 면 [content] 를 그린다.
 * [onRetry] 재시도, [onUseSample] 오프라인 시연용 '샘플로 보기'(있을 때만 노출).
 */
@Composable
fun <T> StateHost(
    state: UiState<T>,
    onRetry: () -> Unit,
    onUseSample: (() -> Unit)? = null,
    content: @Composable (T) -> Unit,
) {
    when (state) {
        is UiState.Loading -> CenterBox {
            CircularProgressIndicator(color = YeobaekTheme.colors.brand, strokeWidth = 2.5.dp, modifier = Modifier.size(30.dp))
        }
        is UiState.Error -> CenterBox {
            Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("여백을 불러오지 못했어요", style = MaterialTheme.typography.titleMedium, color = MaterialTheme.colorScheme.onBackground)
                Text(state.message, color = YeobaekTheme.colors.inkFaint, fontSize = 12.5f.sp, textAlign = TextAlign.Center)
                Text(
                    "다시 시도",
                    color = YeobaekTheme.colors.brand, fontWeight = FontWeight.SemiBold, fontSize = 14.sp,
                    modifier = Modifier.padding(top = 6.dp).clickableNoRipple(onRetry),
                )
                if (onUseSample != null) {
                    Text(
                        "샘플로 보기",
                        color = YeobaekTheme.colors.inkSlate, fontWeight = FontWeight.Medium, fontSize = 13.sp,
                        modifier = Modifier.clickableNoRipple(onUseSample),
                    )
                }
            }
        }
        is UiState.Success -> content(state.data)
    }
}

@Composable
private fun CenterBox(content: @Composable () -> Unit) {
    Box(
        Modifier.fillMaxSize().background(MaterialTheme.colorScheme.background).padding(32.dp),
        contentAlignment = Alignment.Center,
    ) { content() }
}
