package com.example.voicelife.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

@Composable
fun RecordButton(
    enabled: Boolean,
    onPressStart: () -> Unit,
    onPressEnd: () -> Unit,
    onStopSpeaking: (() -> Unit)? = null
) {
    var isPressed by remember { mutableStateOf(false) }

    Box(
        modifier = Modifier
            .size(120.dp)
            .clip(CircleShape)
            .background(
                when {
                    !enabled && onStopSpeaking != null -> Color(0xFFE53935)  // 可中断
                    !enabled -> Color(0xFF9E9E9E)
                    isPressed -> Color(0xFFE53935)
                    else -> MaterialTheme.colorScheme.primary
                }
            )
            .then(
                if (enabled) {
                    Modifier.pointerInput(Unit) {
                        detectTapGestures(onPress = {
                            isPressed = true; onPressStart()
                            tryAwaitRelease()
                            isPressed = false; onPressEnd()
                        })
                    }
                } else if (onStopSpeaking != null) {
                    Modifier.clickable { onStopSpeaking() }
                } else {
                    Modifier
                }
            ),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = when {
                !enabled && onStopSpeaking != null -> "停止"
                !enabled -> "处理中"
                isPressed -> "松开发送"
                else -> "按住说话"
            },
            color = Color.White,
            fontSize = 16.sp,
            textAlign = TextAlign.Center
        )
    }
}
