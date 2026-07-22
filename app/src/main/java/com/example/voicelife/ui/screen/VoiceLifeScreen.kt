package com.example.voicelife.ui.screen

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.example.voicelife.data.model.Role
import com.example.voicelife.ui.components.RecordButton
import com.example.voicelife.viewmodel.PipelineState
import com.example.voicelife.viewmodel.UiMessage
import com.example.voicelife.viewmodel.VoiceLifeViewModel

@Composable
fun VoiceLifeScreen(viewModel: VoiceLifeViewModel) {
    val messages by viewModel.messages.collectAsState()
    val pipelineState by viewModel.pipelineState.collectAsState()
    val statusMessage by viewModel.statusMessage.collectAsState()
    val assistantText by viewModel.assistantText.collectAsState()
    val isStreaming by viewModel.isAssistantStreaming.collectAsState()
    val listState = rememberLazyListState()

    LaunchedEffect(messages.size, isStreaming) {
        listState.animateScrollToItem(listState.layoutInfo.totalItemsCount - 1)
    }

    Scaffold { padding ->
        Column(Modifier.fillMaxSize().padding(padding)) {

            // ── 顶栏 ──
            Surface(Modifier.fillMaxWidth(), color = MaterialTheme.colorScheme.primaryContainer, shadowElevation = 2.dp) {
                Row(Modifier.padding(horizontal = 20.dp, vertical = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text("VoiceLife", fontSize = 20.sp, style = MaterialTheme.typography.titleMedium)
                    Spacer(Modifier.weight(1f))
                    Text(pipelineStateLabel(pipelineState), fontSize = 12.sp, color = MaterialTheme.colorScheme.primary)
                }
            }

            // ── 对话 ──
            LazyColumn(
                Modifier.weight(1f).fillMaxWidth().padding(horizontal = 16.dp),
                state = listState
            ) {
                if (messages.isEmpty() && !isStreaming && statusMessage.isBlank()) {
                    item {
                        Text("按下按钮开始对话", fontSize = 16.sp,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            textAlign = TextAlign.Center,
                            modifier = Modifier.fillMaxWidth().padding(top = 80.dp))
                    }
                }

                items(messages, key = { it.hashCode() }) { msg ->
                    ChatBubble(msg)
                }

                // 实时流式 assistant 文本
                if (isStreaming && assistantText.isNotBlank()) {
                    item {
                        StreamingBubble(assistantText)
                    }
                }

                // 状态消息
                if (statusMessage.isNotBlank() && !isStreaming) {
                    item {
                        Text(statusMessage, fontSize = 13.sp,
                            color = MaterialTheme.colorScheme.primary,
                            modifier = Modifier.padding(vertical = 8.dp, horizontal = 4.dp))
                    }
                }
            }

            // ── 底部按钮 ──
            Surface(Modifier.fillMaxWidth(), shadowElevation = 8.dp, color = MaterialTheme.colorScheme.surface) {
                Column(Modifier.padding(16.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                    RecordButton(
                        enabled = pipelineState == PipelineState.IDLE,
                        onPressStart = viewModel::onPressStart,
                        onPressEnd = viewModel::onPressEnd,
                        onStopSpeaking = if (pipelineState == PipelineState.SPEAKING ||
                            pipelineState == PipelineState.THINKING) viewModel::stopSpeaking else null
                    )
                    Spacer(Modifier.height(8.dp))
                    Text(pipelineHint(pipelineState), fontSize = 12.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun ChatBubble(msg: UiMessage) {
    val isUser = msg.role == Role.USER
    Row(
        Modifier.fillMaxWidth().padding(vertical = 4.dp),
        horizontalArrangement = if (isUser) Arrangement.End else Arrangement.Start
    ) {
        Surface(
            shape = RoundedCornerShape(
                topStart = 16.dp, topEnd = 16.dp,
                bottomStart = if (isUser) 16.dp else 4.dp,
                bottomEnd = if (isUser) 4.dp else 16.dp
            ),
            color = if (isUser) MaterialTheme.colorScheme.primary
                    else MaterialTheme.colorScheme.surfaceVariant,
            modifier = Modifier.widthIn(max = 300.dp)
        ) {
            Column(Modifier.padding(horizontal = 14.dp, vertical = 10.dp)) {
                Text(
                    if (isUser) "你" else "AI", fontSize = 11.sp,
                    color = if (isUser) Color.White.copy(alpha = 0.7f)
                           else MaterialTheme.colorScheme.primary.copy(alpha = 0.7f)
                )
                Spacer(Modifier.height(4.dp))
                Text(msg.text, fontSize = 15.sp,
                    color = if (isUser) Color.White else MaterialTheme.colorScheme.onSurface)
            }
        }
    }
}

@Composable
private fun StreamingBubble(text: String) {
    val infiniteTransition = rememberInfiniteTransition(label = "blink")
    val alpha by infiniteTransition.animateFloat(
        initialValue = 0.3f, targetValue = 0.7f,
        animationSpec = infiniteRepeatable(
            animation = tween(600, easing = LinearEasing),
            repeatMode = RepeatMode.Reverse
        ), label = "cursor"
    )

    Row(Modifier.fillMaxWidth().padding(vertical = 4.dp), horizontalArrangement = Arrangement.Start) {
        Surface(
            shape = RoundedCornerShape(topStart = 16.dp, topEnd = 16.dp, bottomStart = 4.dp, bottomEnd = 16.dp),
            color = MaterialTheme.colorScheme.surfaceVariant,
            modifier = Modifier.widthIn(max = 300.dp)
        ) {
            Column(Modifier.padding(horizontal = 14.dp, vertical = 10.dp)) {
                Text("AI", fontSize = 11.sp,
                    color = MaterialTheme.colorScheme.primary.copy(alpha = 0.7f))
                Spacer(Modifier.height(4.dp))
                Row {
                    Text(text, fontSize = 15.sp, color = MaterialTheme.colorScheme.onSurface)
                    Text(" ▌", fontSize = 15.sp, color = MaterialTheme.colorScheme.primary, modifier = Modifier.alpha(alpha))
                }
            }
        }
    }
}

private fun pipelineStateLabel(state: PipelineState) = when (state) {
    PipelineState.IDLE -> ""
    PipelineState.LISTENING -> "正在聆听..."
    PipelineState.RECOGNIZING -> "语音识别..."
    PipelineState.THINKING -> "AI 思考中..."
    PipelineState.SPEAKING -> "AI 回复中 🔊"
}

private fun pipelineHint(state: PipelineState) = when (state) {
    PipelineState.IDLE -> "按住说话"
    PipelineState.LISTENING -> "松开发送"
    PipelineState.RECOGNIZING -> "识别中..."
    PipelineState.THINKING -> "正在生成回答..."
    PipelineState.SPEAKING -> "点击停止"
}
