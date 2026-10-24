package com.jpo.cellnet.call

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Random

/**
 * Wire format of a live call. The media frame is a binary WebSocket payload: it
 * is never base64 encoded and it can never be confused with a recorded voice
 * note block (0xA5) or with an RS485 bus frame.
 */
class CallProtocolTest {

    @Test
    fun frameConstantsDescribeAnEightKilohertzMonoStream() {
        assertEquals(8000, CallProtocol.SAMPLE_RATE)
        assertEquals(1, CallProtocol.CHANNELS)
        assertEquals(16, CallProtocol.BITS_PER_SAMPLE)
        assertEquals(40, CallProtocol.FRAME_MS)
        // 8 kHz * 40 ms * 2 bytes = 640 bytes, i.e. 25 frames per second.
        assertEquals(640, CallProtocol.FRAME_BYTES)
        assertEquals(16, CallProtocol.HEADER_BYTES)
    }

    @Test
    fun callAudioMagicIsDistinctFromTheRecordedVoiceMagic() {
        // 0xA5 is a recorded voice block pushed by the station; a live call frame
        // must be recognisable on its own.
        assertEquals(0xA7, CallProtocol.MAGIC)
        assertTrue(CallProtocol.isCallFrame(byteArrayOf(0xA7.toByte(), 0x01)))
        assertFalse(CallProtocol.isCallFrame(byteArrayOf(0xA5.toByte(), 0x01)))
        assertFalse(CallProtocol.isCallFrame(ByteArray(0)))
        assertFalse(CallProtocol.isCallFrame(null))
    }

    @Test
    fun generatedCallIdsAreValidAndUnique() {
        val random = Random(42)
        val seen = HashSet<String>()
        repeat(200) {
            val id = CallProtocol.newCallId(random)
            assertTrue(CallProtocol.isValidCallId(id))
            seen.add(id)
        }
        assertEquals(200, seen.size)
    }

    @Test
    fun invalidCallIdsAreRejected() {
        assertFalse(CallProtocol.isValidCallId(null))
        assertFalse(CallProtocol.isValidCallId("short"))
        assertFalse(CallProtocol.isValidCallId("call0001x"))
        assertFalse(CallProtocol.isValidCallId("call 001"))
        assertFalse(CallProtocol.isValidCallId("call-001"))
        assertTrue(CallProtocol.isValidCallId("call0001"))
    }

    @Test
    fun frameRoundTripPreservesIdentityAndPayload() {
        val pcm = ByteArray(CallProtocol.FRAME_BYTES) { index -> (index and 0xFF).toByte() }
        val frame = CallProtocol.buildFrame("call0001", 4242, pcm)
        assertEquals(CallProtocol.HEADER_BYTES + CallProtocol.FRAME_BYTES, frame.size)

        val parsed = CallProtocol.parseFrame(frame)
        assertNotNull(parsed)
        assertEquals("call0001", parsed!!.callId)
        assertEquals(4242, parsed.sequence)
        assertTrue(pcm.contentEquals(parsed.payload))
    }

    @Test
    fun sequenceWrapsOnSixteenBits() {
        val pcm = CallProtocol.silentFrame()
        val frame = CallProtocol.buildFrame("call0001", 70_000, pcm)
        val parsed = CallProtocol.parseFrame(frame)
        assertNotNull(parsed)
        assertEquals(70_000 and 0xFFFF, parsed!!.sequence)
    }

    @Test
    fun aSilentFrameIsAllZeroes() {
        val silence = CallProtocol.silentFrame()
        assertEquals(CallProtocol.FRAME_BYTES, silence.size)
        assertTrue(silence.all { it == 0.toByte() })
    }

    @Test
    fun malformedFramesAreRejected() {
        val pcm = ByteArray(64) { 0x11 }
        val frame = CallProtocol.buildFrame("call0001", 7, pcm)

        // Wrong magic (a recorded voice block, not a call frame).
        assertNull(CallProtocol.parseFrame(frame.copyOf().also { it[0] = 0xA5.toByte() }))
        // Wrong version.
        assertNull(CallProtocol.parseFrame(frame.copyOf().also { it[1] = 0x02 }))
        // Truncated header.
        assertNull(CallProtocol.parseFrame(frame.copyOf(CallProtocol.HEADER_BYTES - 1)))
        // Declared length larger than the payload.
        assertNull(CallProtocol.parseFrame(frame.copyOf().also { it[15] = 0xFF.toByte() }))
        // Declared length smaller than the payload.
        assertNull(
            CallProtocol.parseFrame(
                frame.copyOf().also {
                    it[14] = 0x01
                    it[15] = 0x00
                }
            )
        )
        // Zero length payload.
        assertNull(
            CallProtocol.parseFrame(
                frame.copyOf().also {
                    it[14] = 0x00
                    it[15] = 0x00
                }
            )
        )
        // Non alphanumeric call id.
        assertNull(CallProtocol.parseFrame(frame.copyOf().also { it[4] = ' '.code.toByte() }))
        // Too short to even hold a header.
        assertNull(CallProtocol.parseFrame(ByteArray(4)))
        assertNull(CallProtocol.parseFrame(null))
        // The valid frame still parses after all those refusals.
        assertNotNull(CallProtocol.parseFrame(frame))
    }

    @Test
    fun builderRejectsInvalidInput() {
        val pcm = ByteArray(64) { 0x22 }
        runCatching { CallProtocol.buildFrame("bad", 1, pcm) }.let { assertTrue(it.isFailure) }
        runCatching { CallProtocol.buildFrame("call0001", 1, pcm, 0) }
            .let { assertTrue(it.isFailure) }
        runCatching { CallProtocol.buildFrame("call0001", 1, pcm, pcm.size + 1) }
            .let { assertTrue(it.isFailure) }
        runCatching {
            CallProtocol.buildFrame("call0001", 1, pcm, CallProtocol.MAX_PAYLOAD_BYTES + 1)
        }.let { assertTrue(it.isFailure) }
    }
}
