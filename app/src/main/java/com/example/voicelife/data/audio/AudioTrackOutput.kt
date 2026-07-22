package com.example.voicelife.data.audio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.util.Log
import java.util.concurrent.atomic.AtomicBoolean

class AudioTrackOutput(private val sampleRate: Int = 22050) {
    private val stopped = AtomicBoolean(false)
    private var track: AudioTrack? = null

    fun start() {
        stopped.set(false)
        val bufSize = AudioTrack.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT
        ) * 2
        track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(sampleRate).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(bufSize).setTransferMode(AudioTrack.MODE_STREAM).build()
        track?.play()
        Log.d("Audio", "started " + sampleRate + "Hz")
    }

    fun write(pcm: ByteArray) {
        if (stopped.get()) return
        try {
            val t = track ?: return
            if (t.playState == AudioTrack.PLAYSTATE_PLAYING) t.write(pcm, 0, pcm.size)
        } catch (e: Exception) { Log.e("Audio", "write", e) }
    }

    fun stop() {
        stopped.set(true)
        try { track?.stop() } catch (_: Exception) {}
        try { track?.release() } catch (_: Exception) {}
        track = null
    }
}
