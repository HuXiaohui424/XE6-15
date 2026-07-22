package com.example.voicelife.data.asr

import android.util.Log
import com.google.gson.Gson
import com.google.gson.JsonObject
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

/**
 * DashScope Fun-ASR WebSocket 客户端。
 *
 * 每次调用 [recognize] = 一次完整的 run-task → 收音频 → finish-task 生命周期。
 */
class DashScopeAsrClient private constructor() {

    companion object {
        val Instance: DashScopeAsrClient by lazy { DashScopeAsrClient() }
    }

    private val client = OkHttpClient.Builder()
        .connectTimeout(10, TimeUnit.SECONDS)
        .build()

    private val gson = Gson()

    /**
     * 执行一次语音识别。
     *
     * @param audioQueue 音频数据队列，录音线程往里写，识别结束时会收到一条空的 ByteArray(0) 作为结束信号
     * @param onPartialResult 中间识别结果的回调
     * @return [AsrResult]
     */
    suspend fun recognize(
        audioQueue: ArrayBlockingQueue<ByteArray>,
        onPartialResult: (String) -> Unit
    ): AsrResult = withContext(Dispatchers.IO) {
        val deferred = CompletableDeferred<AsrResult>()

        val (taskId, runTaskMsg) = AsrProtocol.buildRunTask()

        val request = Request.Builder()
            .url(AsrConfig.WEBSOCKET_URL)
            .addHeader("Authorization", "bearer ${AsrConfig.API_KEY}")
            .build()

        val ws = client.newWebSocket(request, object : WebSocketListener() {

            override fun onOpen(webSocket: WebSocket, response: Response) {
                Log.d("ASR", "🔌 connected")
                webSocket.send(runTaskMsg)
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                if (deferred.isCompleted) return
                Log.v("ASR", text)
                try {
                    val json = gson.fromJson(text, JsonObject::class.java)
                    val header = json.getAsJsonObject("header")
                    when (header?.get("event")?.asString) {
                        "task-started" -> startAudioSend(webSocket, audioQueue, taskId)
                        "result-generated" -> handleResult(json.getAsJsonObject("payload"), deferred, onPartialResult)
                        "task-finished" -> { if (!deferred.isCompleted) deferred.complete(AsrResult.Finished) }
                        "task-failed" -> {
                            val err = header?.get("error_message")?.asString ?: "未知错误"
                            if (!deferred.isCompleted) deferred.complete(AsrResult.Error(err))
                        }
                    }
                } catch (e: Exception) {
                    Log.e("ASR", "parse error", e)
                }
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, resp: Response?) {
                Log.e("ASR", "❌ failure: ${t.message}")
                if (!deferred.isCompleted) deferred.complete(AsrResult.Error(t.message ?: "连接失败"))
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                if (!deferred.isCompleted) deferred.complete(AsrResult.Finished)
            }
        })

        val result = deferred.await()
        try { ws?.close(1000, "done") } catch (_: Exception) {}
        result
    }

    private fun handleResult(
        payload: JsonObject?,
        deferred: CompletableDeferred<AsrResult>,
        onPartialResult: (String) -> Unit
    ) {
        val sentence = payload?.getAsJsonObject("output")?.getAsJsonObject("sentence")
        val txt = sentence?.get("text")?.asString ?: ""
        val endTime = sentence?.get("end_time")
        if (endTime != null && !endTime.isJsonNull && txt.isNotBlank()) {
            if (!deferred.isCompleted) deferred.complete(AsrResult.Success(txt))
        } else if (txt.isNotBlank()) {
            onPartialResult(txt)
        }
    }

    private fun startAudioSend(webSocket: WebSocket, queue: ArrayBlockingQueue<ByteArray>, taskId: String) {
        Thread {
            try {
                while (true) {
                    val frame = queue.take()
                    if (frame.isEmpty()) break // 结束信号
                    webSocket.send(ByteString.of(*frame))
                }
            } finally {
                Thread.sleep(300)
                webSocket.send(AsrProtocol.buildFinishTask(taskId))
                Log.d("ASR", "📤 finish-task")
            }
        }.start()
    }
}

sealed class AsrResult {
    data class Success(val text: String) : AsrResult()
    data class Error(val message: String) : AsrResult()
    data object Finished : AsrResult()
}
