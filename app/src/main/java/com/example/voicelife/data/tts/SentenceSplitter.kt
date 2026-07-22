package com.example.voicelife.data.tts

/**
 * 文本 → 句拆分器。
 *
 * 按中文标点 (。！？；\n) 和英文句子边界 (.!?\n) 切分。
 * 每次 feed 一个文本片段，完整句子通过 [onSentence] 回调。
 */
class SentenceSplitter(
    private val onSentence: (String) -> Unit
) {
    private val buffer = StringBuilder()

    fun feed(text: String) {
        buffer.append(text)
        flush()
    }

    /** 结束时清空剩余文本 */
    fun finish() {
        val remaining = buffer.toString().trim()
        if (remaining.isNotBlank()) {
            onSentence(remaining)
        }
        buffer.clear()
    }

    private fun flush() {
        while (true) {
            val idx = findSplitIndex(buffer)
            if (idx < 0) break
            val sentence = buffer.substring(0, idx + 1).trim()
            buffer.delete(0, idx + 1)
            if (sentence.isNotBlank()) {
                onSentence(sentence)
            }
        }
    }

    private fun findSplitIndex(sb: StringBuilder): Int {
        for (i in sb.indices) {
            when (sb[i]) {
                '。', '！', '？', '；', '\n', '.', '!', '?' -> return i
            }
        }
        return -1
    }
}
