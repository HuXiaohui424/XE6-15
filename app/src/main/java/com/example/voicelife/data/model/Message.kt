package com.example.voicelife.data.model

data class Message(
    val role: Role,
    val text: String
)

enum class Role {
    USER, ASSISTANT
}
