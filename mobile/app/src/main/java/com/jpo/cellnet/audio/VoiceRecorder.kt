package com.jpo.cellnet.audio

import android.annotation.SuppressLint
import android.content.Context
import android.media.MediaRecorder
import android.os.SystemClock
import java.io.File

/** Records 60 s maximum voice notes as AAC-LC/M4A files stored on the phone. */
class VoiceRecorder(private val context: Context) {

    data class Clip(val file: File, val durationMs: Long, val bytes: Long)

    private var recorder: MediaRecorder? = null
    private var target: File? = null
    private var startedAt = 0L

    val isRecording: Boolean
        get() = recorder != null

    fun outputDir(): File = File(context.filesDir, "voice").apply { mkdirs() }

    @SuppressLint("MissingPermission")
    fun start(): File {
        require(recorder == null) { "already recording" }
        val file = File(outputDir(), "rec-${System.currentTimeMillis()}.m4a")

        @Suppress("DEPRECATION")
        val media = MediaRecorder()
        media.setAudioSource(MediaRecorder.AudioSource.MIC)
        media.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
        media.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
        media.setAudioChannels(1)
        media.setAudioSamplingRate(16000)
        media.setAudioEncodingBitRate(24000)
        media.setOutputFile(file.absolutePath)
        media.prepare()
        media.start()

        recorder = media
        target = file
        startedAt = SystemClock.elapsedRealtime()
        return file
    }

    /** Stops the recorder and returns the clip, or null when nothing usable was produced. */
    fun stop(): Clip? {
        val media = recorder ?: return null
        val file = target
        val duration = SystemClock.elapsedRealtime() - startedAt
        val stopped = runCatching { media.stop() }.isSuccess
        runCatching { media.release() }
        recorder = null
        target = null

        if (!stopped || file == null || !file.exists() || file.length() == 0L) {
            file?.delete()
            return null
        }
        return Clip(file, duration, file.length())
    }

    fun cancel() {
        val media = recorder ?: return
        runCatching { media.stop() }
        runCatching { media.release() }
        recorder = null
        target?.delete()
        target = null
    }

    fun elapsedMs(): Long =
        if (recorder == null) 0L else SystemClock.elapsedRealtime() - startedAt
}
