package com.jpo.cellnet.audio

import android.media.MediaPlayer
import java.io.File

/** Small MediaPlayer wrapper, one voice note at a time. */
class VoicePlayer {

    private var player: MediaPlayer? = null
    private var playingId: String? = null

    fun playingMessageId(): String? = playingId

    fun play(messageId: String, file: File, onFinished: (String) -> Unit) {
        stop()
        if (!file.exists()) return
        val media = MediaPlayer()
        try {
            media.setDataSource(file.absolutePath)
            media.setOnCompletionListener {
                playingId = null
                runCatching { it.release() }
                player = null
                onFinished(messageId)
            }
            media.setOnErrorListener { _, _, _ ->
                playingId = null
                runCatching { media.release() }
                player = null
                true
            }
            media.prepare()
            media.start()
            player = media
            playingId = messageId
        } catch (error: Exception) {
            runCatching { media.release() }
            player = null
            playingId = null
        }
    }

    fun stop() {
        player?.let { media ->
            runCatching { media.stop() }
            runCatching { media.release() }
        }
        player = null
        playingId = null
    }
}
