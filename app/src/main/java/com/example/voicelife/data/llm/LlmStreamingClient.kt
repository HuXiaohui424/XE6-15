package com.example.voicelife.data.llm

import android.util.Log
import com.example.voicelife.data.asr.AsrConfig
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

object LlmStreamingClient {

    private val URL = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(120, TimeUnit.SECONDS)
        .build()

    fun stream(
        userText: String,
        onChunk: (textDelta: String) -> Unit,
        onDone: (fullText: String) -> Unit,
        onError: (msg: String) -> Unit
    ) {
        val body = JsonObject().apply {
            addProperty("model", "qwen3.6-flash")
            addProperty("stream", true)
            add("messages", com.google.gson.Gson().toJsonTree(listOf(
                mapOf("role" to "user", "content" to userText)
            )))
        }

        val request = Request.Builder()
            .url(URL)
            .addHeader("Authorization", "Bearer ${AsrConfig.API_KEY}")
            .addHeader("Content-Type", "application/json")
            .post(body.toString().toRequestBody("application/json".toMediaType()))
            .build()

        Thread {
            try {
                val response = client.newCall(request).execute()
                if (!response.isSuccessful) {
                    onError("HTTP ${response.code}: ${response.body?.string()}")
                    return@Thread
                }

                val reader = BufferedReader(InputStreamReader(response.body?.byteStream()))
                val fullText = StringBuilder()
                val finished = AtomicBoolean(false)

                reader.forEachLine { line ->
                    if (line.startsWith("data: ") && line.length > 6 && !finished.get()) {
                        val data = line.substring(6)
                        if (data == "[DONE]") {
                            if (finished.compareAndSet(false, true)) {
                                onDone(fullText.toString())
                            }
                            return@forEachLine
                        }
                        try {
                            val json = JsonParser.parseString(data).asJsonObject
                            val choices = json.getAsJsonArray("choices")
                            if (choices != null && choices.size() > 0) {
                                val delta = choices[0].asJsonObject.getAsJsonObject("delta")
                                val content = delta?.get("content")?.asString
                                if (!content.isNullOrEmpty()) {
                                    fullText.append(content)
                                    onChunk(content)
                                }
                            }
                        } catch (_: Exception) {}
                    }
                }
                if (finished.compareAndSet(false, true)) {
                    onDone(fullText.toString())
                }
            } catch (e: Exception) {
                Log.e("LLM", "stream error", e)
                onError(e.message ?: "unknown")
            }
        }.start()
    }
}
