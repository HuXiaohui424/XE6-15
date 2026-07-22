package com.example.voicelife.data.asr

import com.google.gson.JsonObject
import java.util.UUID

/**
 * 协议消息构建器 — DashScope WebSocket run-task / finish-task
 */
object AsrProtocol {

    fun buildRunTask(taskId: String = UUID.randomUUID().toString()): Pair<String, String> {
        val json = basePayload("run-task", taskId).also { root ->
            root.getAsJsonObject("payload").add("parameters", JsonObject().apply {
                addProperty("format", AsrConfig.FORMAT)
                addProperty("sample_rate", AsrConfig.SAMPLE_RATE)
            })
        }
        return taskId to json.toString()
    }

    fun buildFinishTask(taskId: String): String {
        return basePayload("finish-task", taskId).toString()
    }

    private fun basePayload(action: String, taskId: String): JsonObject {
        return JsonObject().apply {
            add("header", JsonObject().apply {
                addProperty("action", action)
                addProperty("task_id", taskId)
                addProperty("streaming", "duplex")
            })
            add("payload", JsonObject().apply {
                addProperty("model", AsrConfig.MODEL)
                addProperty("task_group", "audio")
                addProperty("task", "asr")
                addProperty("function", "recognition")
                add("input", JsonObject())
            })
        }
    }
}
