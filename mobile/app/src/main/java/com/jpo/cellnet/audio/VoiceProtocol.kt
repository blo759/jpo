package com.jpo.cellnet.audio

import java.io.File
import java.util.zip.CRC32

/**
 * Audio parameters shared with the ESP32 transport.
 *
 * Codec choice: AAC-LC in an MPEG-4 container, 16 kHz mono at 24 kbit/s.
 *  * MediaRecorder supports it on every Android 8+ device without extra
 *    dependency (Opus in OGG is only available from API 29 and not on all
 *    devices, AMR-WB is larger for the same quality).
 *  * 24 kbit/s keeps a 60 s recording around 180 kB, which a 115200 baud RS485
 *    bus can carry in roughly 20 s while text messages keep priority.
 */
object VoiceProtocol {
    const val BLOCK_SIZE = 400
    const val MAX_DURATION_MS = 60_000L
    const val MIME = "audio/mp4"
    const val CODEC_LABEL = "AAC-LC 16 kHz mono 24 kbit/s"

    fun crc32(bytes: ByteArray): Long {
        val crc = CRC32()
        crc.update(bytes)
        return crc.value
    }

    fun crc32(file: File): Long {
        val crc = CRC32()
        file.inputStream().use { stream ->
            val buffer = ByteArray(8 * 1024)
            while (true) {
                val read = stream.read(buffer)
                if (read <= 0) break
                crc.update(buffer, 0, read)
            }
        }
        return crc.value
    }

    fun blockCount(bytes: Long): Int =
        if (bytes <= 0L) 0 else ((bytes + BLOCK_SIZE - 1) / BLOCK_SIZE).toInt()
}
