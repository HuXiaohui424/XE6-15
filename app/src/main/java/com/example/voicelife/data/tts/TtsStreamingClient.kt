package com.example.voicelife.data.tts

import android.util.Log
import com.alibaba.dashscope.audio.tts.SpeechSynthesisResult
import com.alibaba.dashscope.audio.ttsv2.SpeechSynthesisAudioFormat
import com.alibaba.dashscope.audio.ttsv2.SpeechSynthesisParam
import com.alibaba.dashscope.audio.ttsv2.SpeechSynthesizer
import com.alibaba.dashscope.common.ResultCallback
import com.example.voicelife.data.asr.AsrConfig
import java.util.concurrent.CountDownLatch

/**
 * LLM 实时流 → TTS → 音频回调。
 *
 * 用法:
 *   val tts = TtsStreamingClient(onAudio = { pcm -> player.write(pcm) })
 *   tts.feed("你好")        // LLM 每产出一个 text delta 就喂进去
 *   tts.feed("，我是AI")    // 继续喂
 *   tts.complete()          // LLM 流结束，等待 TTS 收尾
 */
class TtsStreamingClient(
    private val onAudio: (ByteArray) -> Unit,
    private val onCompleted: () -> Unit,
    private val onError: (String) -> Unit
) {
    private var synthesizer: SpeechSynthesizer? = null
    private var started = false

    private fun ensureStarted() {
        if (started) return
        started = true

        val param = SpeechSynthesisParam.builder()
            .apiKey(AsrConfig.API_KEY)
            .model("qwen-audio-3.0-tts-flash")
            .voice("longanhuan_v3.6")
            .format(SpeechSynthesisAudioFormat.PCM_22050HZ_MONO_16BIT)
            .build()

        synthesizer = SpeechSynthesizer(param, object : ResultCallback<SpeechSynthesisResult>() {
            override fun onEvent(result: SpeechSynthesisResult) {
                val frame = result.audioFrame
                if (frame != null && frame.hasRemaining()) {
                    val bytes = ByteArray(frame.remaining())
                    frame.get(bytes)
                    onAudio(bytes)
                }
            }

            override fun onComplete() {
                Log.d("TTS", "✅ complete")
                onCompleted()
            }

            override fun onError(e: Exception) {
                Log.e("TTS", "❌ ${e.message}", e)
                onError(e.message ?: "TTS error")
            }
        })
    }

    /** LLM 每输出一个 text delta 就调用一次 */
    fun feed(text: String) {
        ensureStarted()
        try {
            synthesizer?.streamingCall(text)
        } catch (e: Exception) {
            Log.e("TTS", "feed error", e)
        }
    }

    /** LLM 流结束，发送完成信号 */
    fun complete() {
        try {
            synthesizer?.streamingComplete()
        } catch (e: Exception) {
            Log.e("TTS", "complete error", e)
        }
    }

    /** 取消当前任务 */
    fun cancel() {
        try {
            synthesizer?.streamingCancel()
            synthesizer?.getDuplexApi()?.close(1000, "cancel")
        } catch (_: Exception) {}
    }

    fun close() {
        try { synthesizer?.getDuplexApi()?.close(1000, "done") } catch (_: Exception) {}
    }
}
