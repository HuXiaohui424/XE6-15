package com.example.voicelife.viewmodel

import android.app.Application
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.example.voicelife.data.asr.AsrResult
import com.example.voicelife.data.asr.DashScopeAsrClient
import com.example.voicelife.data.audio.AudioTrackOutput
import com.example.voicelife.data.audio.AudioRecorder
import com.example.voicelife.data.llm.LlmStreamingClient
import com.example.voicelife.data.model.Role
import com.example.voicelife.data.tts.TtsStreamingClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlin.coroutines.resume

enum class PipelineState { IDLE, LISTENING, RECOGNIZING, THINKING, SPEAKING }

class VoiceLifeViewModel(app: Application) : AndroidViewModel(app) {
    private val asrClient = DashScopeAsrClient.Instance
    private var audioRecorder: AudioRecorder? = null
    private var player: AudioTrackOutput? = null
    private var tts: TtsStreamingClient? = null

    private val _pipelineState = MutableStateFlow(PipelineState.IDLE)
    val pipelineState: StateFlow<PipelineState> = _pipelineState.asStateFlow()
    private val _statusMessage = MutableStateFlow("")
    val statusMessage: StateFlow<String> = _statusMessage.asStateFlow()
    private val _assistantText = MutableStateFlow("")
    val assistantText: StateFlow<String> = _assistantText.asStateFlow()
    private val _messages = MutableStateFlow<List<UiMessage>>(emptyList())
    val messages: StateFlow<List<UiMessage>> = _messages.asStateFlow()
    private val _isAssistantStreaming = MutableStateFlow(false)
    val isAssistantStreaming: StateFlow<Boolean> = _isAssistantStreaming.asStateFlow()

    fun onPressStart() {
        if (_pipelineState.value != PipelineState.IDLE) return
        _pipelineState.value = PipelineState.LISTENING
        _statusMessage.value = ""
        _assistantText.value = ""
        _isAssistantStreaming.value = false

        val recorder = AudioRecorder()
        audioRecorder = recorder

        viewModelScope.launch {
            withContext(Dispatchers.IO) { recorder.start() }
            _pipelineState.value = PipelineState.RECOGNIZING

            val asrResult = withContext(Dispatchers.IO) {
                asrClient.recognize(recorder.audioQueue) { _statusMessage.value = it }
            }
            val userText = when (asrResult) {
                is AsrResult.Success -> asrResult.text
                else -> { _pipelineState.value = PipelineState.IDLE; return@launch }
            }
            _messages.value = _messages.value + UiMessage(Role.USER, userText, false)

            _pipelineState.value = PipelineState.THINKING
            _statusMessage.value = "AI ..."
            _isAssistantStreaming.value = true
            _assistantText.value = ""

            // === 核心：LLM stream -> TTS direct -> AudioTrack ===
            player = AudioTrackOutput(22050)
            player!!.start()

            withContext(Dispatchers.IO) {
                suspendCancellableCoroutine<Unit> { cont ->
                    tts = TtsStreamingClient(
                        onAudio = { pcm -> player?.write(pcm) },
                        onCompleted = { cont.resume(Unit) },
                        onError = { cont.resume(Unit) }
                    )
                    LlmStreamingClient.stream(
                        userText = userText,
                        onChunk = { delta ->
                            _assistantText.value += delta
                            tts?.feed(delta)
                            if (_pipelineState.value == PipelineState.THINKING) {
                                _pipelineState.value = PipelineState.SPEAKING
                                _statusMessage.value = "AI ..."
                            }
                        },
                        onDone = { fullText ->
                            tts?.complete()
                            _messages.value = _messages.value + UiMessage(Role.ASSISTANT, fullText, false)
                            _assistantText.value = ""
                        },
                        onError = { err ->
                            _messages.value = _messages.value + UiMessage(Role.ASSISTANT, "err: ", false)
                            cont.resume(Unit)
                        }
                    )
                }
            }

            player?.stop()
            _isAssistantStreaming.value = false
            _pipelineState.value = PipelineState.IDLE
            _statusMessage.value = ""
        }
    }

    fun onPressEnd() { audioRecorder?.stop() }
    fun stopSpeaking() {
        tts?.cancel()
        player?.stop()
        _pipelineState.value = PipelineState.IDLE
    }
    override fun onCleared() {
        super.onCleared()
        audioRecorder?.release()
        tts?.close()
        player?.stop()
    }
}

data class UiMessage(val role: Role, val text: String, val isStreaming: Boolean)
