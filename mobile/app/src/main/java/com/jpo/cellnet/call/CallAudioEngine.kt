package com.jpo.cellnet.call

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.Build
import android.util.Log
import kotlin.concurrent.thread

/**
 * Bidirectional audio path of a live call: microphone capture and speaker
 * playback, both at [CallProtocol.SAMPLE_RATE] mono 16 bit, in frames of
 * [CallProtocol.FRAME_MS] ms.
 *
 * The engine knows nothing about the network: it hands captured frames to the
 * caller and plays the frames the caller enqueues. That keeps the transport
 * (binary WebSocket relayed by the local ESP32) fully replaceable and testable,
 * and it means a call never touches the RS485 bus.
 *
 * Two threads only, both bound to the lifetime of the call, and both released by
 * [stop]. A muted participant keeps sending silence so the peer hears silence
 * instead of a broken stream, and so the station liveness watchdog stays happy.
 */
class CallAudioEngine(private val context: Context) {

    interface Listener {
        /** A captured frame, ready to be sent as one binary WebSocket message. */
        fun onMicrophoneFrame(frame: ByteArray, length: Int)

        /** Capture or playback failure: the call must be ended. */
        fun onAudioError(message: String)
    }

    private val audioManager =
        context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private var record: AudioRecord? = null
    private var track: AudioTrack? = null
    private var focusRequest: AudioFocusRequest? = null

    @Volatile
    private var running = false

    @Volatile
    private var muted = false

    @Volatile
    private var captureSequence = 0

    @Volatile
    private var lastFrameAt = 0L

    @Volatile
    private var received = 0

    private var captureThread: Thread? = null
    private var playbackThread: Thread? = null

    private val jitter = JitterBuffer()

    val isRunning: Boolean get() = running
    val isMuted: Boolean get() = muted
    val receivedFrames: Int get() = received

    /** Milliseconds since the last incoming frame, or -1 when none arrived yet. */
    fun silenceMs(now: Long): Long {
        val last = lastFrameAt
        return if (last == 0L) -1L else now - last
    }

    /**
     * Opens the microphone and the speaker. Returns false when the platform
     * refuses to build one of them (permission revoked, device busy).
     */
    @SuppressLint("MissingPermission")
    fun start(listener: Listener, speaker: Boolean = true): Boolean {
        if (running) return true
        val minRecord = AudioRecord.getMinBufferSize(
            CallProtocol.SAMPLE_RATE,
            AudioFormat.CHANNEL_IN_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        val minTrack = AudioTrack.getMinBufferSize(
            CallProtocol.SAMPLE_RATE,
            AudioFormat.CHANNEL_OUT_MONO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        if (minRecord <= 0 || minTrack <= 0) {
            Log.e(TAG, "unsupported audio configuration (record=$minRecord track=$minTrack)")
            return false
        }

        val recordBuffer = maxOf(minRecord, CallProtocol.FRAME_BYTES * 4)
        val recorder = runCatching {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                CallProtocol.SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                recordBuffer
            )
        }.getOrNull() ?: runCatching {
            // Some devices refuse VOICE_COMMUNICATION without a telephony stack.
            AudioRecord(
                MediaRecorder.AudioSource.MIC,
                CallProtocol.SAMPLE_RATE,
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT,
                recordBuffer
            )
        }.getOrNull()
        if (recorder == null || recorder.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord unavailable")
            runCatching { recorder?.release() }
            return false
        }

        val player = runCatching {
            AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(CallProtocol.SAMPLE_RATE)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                        .build()
                )
                .setBufferSizeInBytes(maxOf(minTrack, CallProtocol.FRAME_BYTES * 4))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
        }.getOrNull()
        if (player == null || player.state != AudioTrack.STATE_INITIALIZED) {
            Log.e(TAG, "AudioTrack unavailable")
            runCatching { player?.release() }
            recorder.release()
            return false
        }

        record = recorder
        track = player
        muted = false
        received = 0
        lastFrameAt = 0L
        captureSequence = 0
        jitter.reset()

        requestAudioFocus()
        applyRoute(speaker)

        running = true
        recorder.startRecording()
        player.play()

        captureThread = thread(name = "cellnet-call-capture") { captureLoop(recorder, listener) }
        playbackThread = thread(name = "cellnet-call-playback") { playbackLoop(player) }
        Log.i(
            TAG,
            "call audio started (${CallProtocol.SAMPLE_RATE} Hz, " +
                "${CallProtocol.FRAME_MS} ms frames, ${CallProtocol.FRAME_BYTES} bytes)"
        )
        return true
    }

    /** Queues one incoming media frame for playback. */
    fun enqueueIncoming(frame: ByteArray) {
        val parsed = CallProtocol.parseFrame(frame) ?: return
        if (!running) return
        received++
        lastFrameAt = System.currentTimeMillis()
        jitter.push(parsed.sequence, parsed.payload)
    }

    /** A muted participant still sends silence: the peer must not hear a cut. */
    fun setMuted(value: Boolean) {
        muted = value
    }

    fun setSpeaker(enabled: Boolean) {
        applyRoute(enabled)
    }

    fun isSpeakerOn(): Boolean = runCatching { audioManager.isSpeakerphoneOn }.getOrDefault(false)

    /** Releases every audio resource. Safe to call twice. */
    fun stop() {
        if (!running && record == null && track == null) {
            jitter.reset()
            return
        }
        running = false
        captureThread?.let { runCatching { it.join(500) } }
        playbackThread?.let { runCatching { it.join(500) } }
        captureThread = null
        playbackThread = null

        record?.let { recorder ->
            runCatching { recorder.stop() }
            runCatching { recorder.release() }
        }
        record = null
        track?.let { player ->
            runCatching { player.stop() }
            runCatching { player.release() }
        }
        track = null

        abandonAudioFocus()
        runCatching { audioManager.mode = AudioManager.MODE_NORMAL }
        Log.i(
            TAG,
            "call audio released (received=$received, dropped=${jitter.droppedFrames}, " +
                "silences=${jitter.filledSilences})"
        )
        jitter.reset()
    }

    private fun captureLoop(recorder: AudioRecord, listener: Listener) {
        val buffer = ByteArray(CallProtocol.FRAME_BYTES)
        val silence = CallProtocol.silentFrame()
        while (running) {
            val read = runCatching {
                recorder.read(buffer, 0, CallProtocol.FRAME_BYTES)
            }.getOrDefault(0)
            if (!running) return
            if (read < 0) {
                Log.w(TAG, "AudioRecord.read failed ($read)")
                listener.onAudioError("microphone indisponible")
                return
            }
            if (read < CallProtocol.FRAME_BYTES) {
                continue
            }
            // Muting is handled here, not by stopping the capture: the stream
            // keeps its cadence and the peer receives silence frames.
            val payload = if (muted) silence else buffer
            listener.onMicrophoneFrame(payload, CallProtocol.FRAME_BYTES)
        }
    }

    private fun playbackLoop(player: AudioTrack) {
        val out = ByteArray(CallProtocol.FRAME_BYTES)
        val silence = CallProtocol.silentFrame()
        while (running) {
            val action = jitter.pull(out)
            val source = if (action == JitterBuffer.Pull.FRAME) out else silence
            // write() blocks while the track buffer is full, which paces the loop
            // at exactly one frame per 40 ms and keeps the latency bounded.
            val written = runCatching {
                player.write(source, 0, CallProtocol.FRAME_BYTES)
            }.getOrDefault(-1)
            if (!running) return
            if (written < 0) {
                Log.w(TAG, "AudioTrack.write failed ($written)")
                return
            }
        }
    }

    private fun applyRoute(speaker: Boolean) {
        runCatching { audioManager.mode = AudioManager.MODE_IN_COMMUNICATION }
        runCatching { audioManager.isSpeakerphoneOn = speaker }
    }

    private fun requestAudioFocus() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
            .build()
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
            .setAudioAttributes(attributes)
            .setAcceptsDelayedFocusGain(false)
            .setOnAudioFocusChangeListener { change ->
                Log.i(TAG, "audio focus changed: $change")
            }
            .build()
        focusRequest = request
        runCatching { audioManager.requestAudioFocus(request) }
    }

    private fun abandonAudioFocus() {
        val request = focusRequest ?: return
        focusRequest = null
        runCatching { audioManager.abandonAudioFocusRequest(request) }
    }

    companion object {
        private const val TAG = "CellNetCall"
    }
}
