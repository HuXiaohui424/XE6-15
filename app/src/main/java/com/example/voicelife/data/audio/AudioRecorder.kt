package com.example.voicelife.data.audio

import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * 16kHz 16-bit mono PCM 录音器。
 *
 * 录音数据写入 [audioQueue]，停止时发送一帧空的 ByteArray(0) 作为结束信号。
 */
class AudioRecorder(
    private val sampleRate: Int = 16000,
    private val frameSize: Int = 1600  // 50ms
) {
    val audioQueue = ArrayBlockingQueue<ByteArray>(100)

    private var recorder: AudioRecord? = null
    private var readThread: Thread? = null
    private val running = AtomicBoolean(false)

    /** 开始录音（在主线程调用创建，在子线程启动读取循环） */
    fun start(onStarted: () -> Unit = {}) {
        val bufferSize = AudioRecord.getMinBufferSize(
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        ).coerceAtLeast(frameSize)

        recorder = AudioRecord(
            MediaRecorder.AudioSource.MIC,
            sampleRate,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT,
            bufferSize
        )
        recorder?.startRecording()
        running.set(true)

        readThread = Thread {
            onStarted()
            val buffer = ByteArray(frameSize)
            while (running.get()) {
                val read = recorder?.read(buffer, 0, buffer.size) ?: -1
                if (read > 0) {
                    audioQueue.offer(buffer.copyOf(read))
                }
            }
            // 发送结束信号
            audioQueue.offer(ByteArray(0))
            try { recorder?.stop() } catch (_: Exception) {}
            try { recorder?.release() } catch (_: Exception) {}
            recorder = null
        }.apply { start() }
    }

    /** 停止录音 */
    fun stop() {
        running.set(false)
    }

    /** 强制释放资源 */
    fun release() {
        running.set(false)
        try { recorder?.stop() } catch (_: Exception) {}
        try { recorder?.release() } catch (_: Exception) {}
        recorder = null
    }
}
