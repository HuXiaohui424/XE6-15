package com.example.voicelife.data.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class AudioQueuePlayer {

    private val queue = LinkedBlockingQueue<ByteArray>(100)
    private val stopped = AtomicBoolean(false)
    private val running = AtomicBoolean(false)
    private val started = AtomicBoolean(false)
    private var worker: Thread? = null
    @Volatile private var track: AudioTrack? = null

    fun enqueue(pcm: ByteArray) {
        if (!stopped.get()) queue.offer(pcm)
    }

    fun start() {
        if (started.getAndSet(true)) return
        stopped.set(false)
        running.set(true)

        worker = Thread {
            try {
                val bufSize = AudioTrack.getMinBufferSize(
                    22050, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
                ) * 2

                track = AudioTrack.Builder()
                    .setAudioAttributes(
                        AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                            .build()
                    )
                    .setAudioFormat(
                        AudioFormat.Builder()
                            .setSampleRate(22050)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .build()
                    )
                    .setBufferSizeInBytes(bufSize)
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build()

                track?.play()
                Log.d("Audio", "🎵 22050Hz STARTED")
                running.set(true)

                while (running.get()) {
                    val chunk = queue.poll(300, TimeUnit.MILLISECONDS) ?: continue
                    if (chunk.isEmpty()) continue
                    // TTS SDK 返回的可能是 WAV(RIFF头) 或纯 PCM，处理两种
                    val data = if (chunk.size > 44 &&
                        chunk[0] == 'R'.code.toByte() &&
                        chunk[1] == 'I'.code.toByte() &&
                        chunk[2] == 'F'.code.toByte()
                    ) chunk.copyOfRange(44, chunk.size) else chunk
                    if (data.isEmpty()) continue
                    try {
                        val t = track ?: break
                        if (t.playState == AudioTrack.PLAYSTATE_PLAYING) {
                            t.write(data, 0, data.size)
                        }
                    } catch (e: Exception) {
                        if (!stopped.get()) Log.e("Audio", "write", e)
                        break
                    }
                }
                while (true) {
                    val chunk = queue.poll() ?: break
                    if (chunk.isEmpty()) continue
                    val data = if (chunk.size > 44 &&
                        chunk[0] == 'R'.code.toByte() &&
                        chunk[1] == 'I'.code.toByte()
                    ) chunk.copyOfRange(44, chunk.size) else chunk
                    if (data.isEmpty()) continue
                    try {
                        val t = track
                        if (t != null && t.playState == AudioTrack.PLAYSTATE_PLAYING) {
                            t.write(data, 0, data.size)
                        }
                    } catch (_: Exception) {}
                }
                Thread.sleep(500)
            } catch (e: Exception) {
                if (!stopped.get()) Log.e("Audio", "crash", e)
            } finally {
                try { track?.stop() } catch (_: Exception) {}
                try { track?.release() } catch (_: Exception) {}
                track = null
                started.set(false)
                Log.d("Audio", "done")
            }
        }.apply { name = "audio"; start() }
    }

    fun stop() {
        stopped.set(true)
        running.set(false)
        try { track?.stop() } catch (_: Exception) {}
    }
}
