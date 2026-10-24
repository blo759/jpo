package com.jpo.cellnet.net

/**
 * Binary voice frame pushed by the station on the WebSocket, right after the
 * JSON header:
 *
 *   [0xA5][0x01][msgId:16][seq:2 LE][total:2 LE][audio bytes...]
 *
 * The block identity is repeated inside the binary frame on purpose. The previous
 * implementation remembered the last JSON header in a single volatile field, so
 * a lost header (or two headers arriving before their payloads) could associate
 * the audio bytes with the wrong block, or with the wrong voice note entirely.
 * With this envelope the receiver always knows what it just received.
 *
 * Pure Kotlin: unit tested on the JVM without Android.
 */
object BinaryVoiceFrame {

    const val MAGIC = 0xA5
    const val VERSION = 0x01
    const val OVERHEAD = 22

    data class Block(val msgId: String, val seq: Int, val total: Int, val data: ByteArray) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is Block) return false
            return msgId == other.msgId && seq == other.seq && total == other.total &&
                data.contentEquals(other.data)
        }

        override fun hashCode(): Int {
            var result = msgId.hashCode()
            result = 31 * result + seq
            result = 31 * result + total
            result = 31 * result + data.contentHashCode()
            return result
        }
    }

    fun encode(msgId: String, seq: Int, total: Int, data: ByteArray): ByteArray {
        require(msgId.length == 16) { "msgId must be 16 characters" }
        val out = ByteArray(OVERHEAD + data.size)
        out[0] = MAGIC.toByte()
        out[1] = VERSION.toByte()
        val idBytes = msgId.toByteArray(Charsets.US_ASCII)
        System.arraycopy(idBytes, 0, out, 2, 16)
        out[18] = (seq and 0xFF).toByte()
        out[19] = ((seq shr 8) and 0xFF).toByte()
        out[20] = (total and 0xFF).toByte()
        out[21] = ((total shr 8) and 0xFF).toByte()
        System.arraycopy(data, 0, out, OVERHEAD, data.size)
        return out
    }

    /** Returns null when the frame is not a valid voice block envelope. */
    fun decode(bytes: ByteArray): Block? {
        if (bytes.size <= OVERHEAD) return null
        if ((bytes[0].toInt() and 0xFF) != MAGIC) return null
        if ((bytes[1].toInt() and 0xFF) != VERSION) return null
        val msgId = String(bytes, 2, 16, Charsets.US_ASCII)
        if (msgId.isBlank()) return null
        val seq = (bytes[18].toInt() and 0xFF) or ((bytes[19].toInt() and 0xFF) shl 8)
        val total = (bytes[20].toInt() and 0xFF) or ((bytes[21].toInt() and 0xFF) shl 8)
        if (seq < 0 || total <= 0 || seq >= total) return null
        val data = bytes.copyOfRange(OVERHEAD, bytes.size)
        if (data.isEmpty()) return null
        return Block(msgId, seq, total, data)
    }
}
