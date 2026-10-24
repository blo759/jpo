package com.jpo.cellnet.call

import java.util.Random

/**
 * Wire format of a live call, shared with the ESP32 station.
 *
 * A call is **not** a recorded voice note: the audio is a continuous
 * bidirectional PCM stream relayed by the local station between the two phones
 * of the same cell, as binary WebSocket frames. Nothing is buffered on the
 * station and nothing is ever stored on it.
 *
 * Frame layout (binary WebSocket payload):
 * ```
 *   offset size field
 *   0      1    magic 0xA7 (live call audio, distinct from 0xA5 voice notes)
 *   1      1    version (1)
 *   2      2    flags, reserved, 0
 *   4      8    call id, ASCII alphanumeric
 *   12     2    sequence, little endian, per direction
 *   14     2    payload length, little endian
 *   16     n    PCM payload (16 bit signed, mono, 8 kHz)
 * ```
 * 8 kHz mono 16 bit with 40 ms frames is 640 bytes: 25 frames per second and per
 * direction, which keeps the ESP32 relay and the Wi-Fi link comfortable.
 *
 * Pure Kotlin: unit tested on the JVM without Android.
 */
object CallProtocol {

    const val SAMPLE_RATE = 8_000
    const val CHANNELS = 1
    const val BITS_PER_SAMPLE = 16
    const val FRAME_MS = 40
    const val FRAME_BYTES = SAMPLE_RATE * FRAME_MS / 1000 * (BITS_PER_SAMPLE / 8)

    const val MAGIC = 0xA7
    const val VERSION = 1
    const val HEADER_BYTES = 16
    const val MAX_PAYLOAD_BYTES = 1024
    const val CALL_ID_LENGTH = 8

    /** Invitation lifetime, identical to the station side. */
    const val RING_TIMEOUT_MS = 30_000L

    /** Minimum delay between two outgoing invitations (station side guard too). */
    const val INVITE_COOLDOWN_MS = 1_500L

    /** Playback starts once this many frames are queued (2 x 40 ms). */
    const val JITTER_TARGET_FRAMES = 2

    /** Hard bound of the jitter buffer: 12 frames = 480 ms. */
    const val JITTER_MAX_FRAMES = 12

    /** A gap is filled with silence once that many frames are waiting behind it. */
    const val GAP_THRESHOLD_FRAMES = 3

    /** No media at all for this long: the call is considered lost. */
    const val MEDIA_SILENCE_TIMEOUT_MS = 8_000L

    private const val CALL_ID_ALPHABET = "0123456789abcdefghijklmnopqrstuvwxyz"

    /** A frame payload is 640 bytes of PCM; a silent one keeps the stream alive. */
    fun silentFrame(): ByteArray = ByteArray(FRAME_BYTES)

    fun newCallId(random: Random = Random()): String = buildString(CALL_ID_LENGTH) {
        repeat(CALL_ID_LENGTH) { append(CALL_ID_ALPHABET[random.nextInt(CALL_ID_ALPHABET.length)]) }
    }

    fun isValidCallId(callId: String?): Boolean {
        if (callId == null || callId.length != CALL_ID_LENGTH) return false
        return callId.all { it.isDigit() || (it in 'a'..'z') || (it in 'A'..'Z') }
    }

    /** True when a binary WebSocket payload is a live call media frame. */
    fun isCallFrame(bytes: ByteArray?): Boolean =
        bytes != null && bytes.isNotEmpty() && bytes[0] == MAGIC.toByte()

    data class Frame(val callId: String, val sequence: Int, val payload: ByteArray) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is Frame) return false
            return callId == other.callId && sequence == other.sequence &&
                payload.contentEquals(other.payload)
        }

        override fun hashCode(): Int {
            var result = callId.hashCode()
            result = 31 * result + sequence
            result = 31 * result + payload.contentHashCode()
            return result
        }
    }

    /**
     * Encodes one media frame. [length] allows sending a shorter last frame (the
     * capture callback always fills a whole frame, but the encoder is generic).
     */
    fun buildFrame(callId: String, sequence: Int, pcm: ByteArray, length: Int = pcm.size): ByteArray {
        require(isValidCallId(callId)) { "invalid call id" }
        require(length in 1..MAX_PAYLOAD_BYTES) { "invalid payload length: $length" }
        require(length <= pcm.size) { "declared length exceeds the payload" }
        val out = ByteArray(HEADER_BYTES + length)
        out[0] = MAGIC.toByte()
        out[1] = VERSION.toByte()
        // bytes 2..3: flags, reserved
        for (index in 0 until CALL_ID_LENGTH) {
            out[4 + index] = callId[index].code.toByte()
        }
        val sequence16 = sequence and 0xFFFF
        out[12] = (sequence16 and 0xFF).toByte()
        out[13] = ((sequence16 shr 8) and 0xFF).toByte()
        out[14] = (length and 0xFF).toByte()
        out[15] = ((length shr 8) and 0xFF).toByte()
        System.arraycopy(pcm, 0, out, HEADER_BYTES, length)
        return out
    }

    /** Decodes a media frame, or null when it is malformed. */
    fun parseFrame(bytes: ByteArray?): Frame? {
        if (bytes == null || bytes.size < HEADER_BYTES) return null
        if (bytes[0] != MAGIC.toByte() || bytes[1] != VERSION.toByte()) return null
        val callId = String(bytes, 4, CALL_ID_LENGTH, Charsets.US_ASCII)
        if (!isValidCallId(callId)) return null
        val declared = (bytes[14].toInt() and 0xFF) or ((bytes[15].toInt() and 0xFF) shl 8)
        if (declared <= 0 || declared > MAX_PAYLOAD_BYTES) return null
        if (bytes.size != HEADER_BYTES + declared) return null
        val sequence = (bytes[12].toInt() and 0xFF) or ((bytes[13].toInt() and 0xFF) shl 8)
        return Frame(callId, sequence, bytes.copyOfRange(HEADER_BYTES, bytes.size))
    }
}
