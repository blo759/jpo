package com.jpo.cellnet.net

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Binary voice frame envelope. The regression this file protects against is the
 * mixing of two voice notes: the previous implementation remembered the last JSON
 * header in a single field, so a lost header made the audio bytes land on the
 * wrong block.
 */
class BinaryVoiceFrameTest {

    // 16 characters, exactly like UserProfileStore/AppViewModel.newId():
    // "m-" followed by 14 characters. The wire format carries a fixed 16 byte id.
    private val msgId = "m-0123456789abcd"

    @Test
    fun roundTripKeepsIdentityAndPayload() {
        val payload = ByteArray(400) { (it % 251).toByte() }
        val encoded = BinaryVoiceFrame.encode(msgId, 12, 455, payload)
        assertEquals(BinaryVoiceFrame.OVERHEAD + payload.size, encoded.size)

        val decoded = BinaryVoiceFrame.decode(encoded)
        requireNotNull(decoded)
        assertEquals(msgId, decoded.msgId)
        assertEquals(12, decoded.seq)
        assertEquals(455, decoded.total)
        assertArrayEquals(payload, decoded.data)
    }

    @Test
    fun twoInterleavedBlocksNeverGetMixedUp() {
        // This is the exact scenario that used to be ambiguous: two headers
        // followed by two payloads, in the opposite order.
        val first = BinaryVoiceFrame.encode("m-aaaaaaaaaaaaaa", 3, 10, ByteArray(16) { 1 })
        val second = BinaryVoiceFrame.encode("m-bbbbbbbbbbbbbb", 7, 20, ByteArray(16) { 2 })

        val decodedSecond = BinaryVoiceFrame.decode(second)
        val decodedFirst = BinaryVoiceFrame.decode(first)
        requireNotNull(decodedSecond)
        requireNotNull(decodedFirst)
        assertEquals("m-bbbbbbbbbbbbbb", decodedSecond.msgId)
        assertEquals(7, decodedSecond.seq)
        assertEquals("m-aaaaaaaaaaaaaa", decodedFirst.msgId)
        assertEquals(3, decodedFirst.seq)
        assertEquals(1, decodedFirst.data[0].toInt())
        assertEquals(2, decodedSecond.data[0].toInt())
    }

    @Test
    fun wrongMessageIdLengthIsRejected() {
        // The wire format reserves exactly 16 bytes for the message id.
        try {
            BinaryVoiceFrame.encode("m-too-short", 0, 1, ByteArray(8))
            throw AssertionError("a short message id must be refused")
        } catch (expected: IllegalArgumentException) {
            // expected
        }
    }

    @Test
    fun legacyFrameWithoutMagicIsRejected() {
        // A raw audio frame from the previous firmware version must be refused so
        // the caller can fall back to the JSON header correlation.
        assertNull(BinaryVoiceFrame.decode(ByteArray(64) { 0x11 }))
        assertNull(BinaryVoiceFrame.decode(ByteArray(4)))
    }

    @Test
    fun inconsistentSequenceIsRejected() {
        assertNull(BinaryVoiceFrame.decode(BinaryVoiceFrame.encode(msgId, 5, 5, ByteArray(8))))
        assertNull(BinaryVoiceFrame.decode(BinaryVoiceFrame.encode(msgId, 9, 2, ByteArray(8))))
    }

    @Test
    fun emptyPayloadIsRejected() {
        assertNull(BinaryVoiceFrame.decode(BinaryVoiceFrame.encode(msgId, 0, 1, ByteArray(0))))
    }
}
