package com.jpo.cellnet.call

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Playback buffer of the incoming call stream: reordering, gap filling and
 * bounded latency. Pure JVM, no Android.
 */
class JitterBufferTest {

    private val frameBytes = 8
    private fun frame(value: Int) = ByteArray(frameBytes) { value.toByte() }

    @Test
    fun inOrderFramesArePlayedInOrder() {
        val buffer = JitterBuffer(frameBytes)
        buffer.push(0, frame(1))
        buffer.push(1, frame(2))
        val out = ByteArray(frameBytes)
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(1.toByte(), out[0])
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(2.toByte(), out[0])
        assertEquals(0, buffer.droppedFrames)
        assertEquals(0, buffer.filledSilences)
    }

    @Test
    fun outOfOrderFramesAreReordered() {
        val buffer = JitterBuffer(frameBytes)
        buffer.push(0, frame(1))
        buffer.push(2, frame(3))
        buffer.push(1, frame(2))
        val out = ByteArray(frameBytes)
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(1.toByte(), out[0])
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(2.toByte(), out[0])
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(3.toByte(), out[0])
    }

    @Test
    fun anEmptyBufferAsksTheCallerToWait() {
        val buffer = JitterBuffer(frameBytes)
        assertEquals(JitterBuffer.Pull.EMPTY, buffer.pull(ByteArray(frameBytes)))
        buffer.push(0, frame(1))
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(ByteArray(frameBytes)))
        assertEquals(JitterBuffer.Pull.EMPTY, buffer.pull(ByteArray(frameBytes)))
    }

    @Test
    fun theStreamStartsOnTheFirstFrameReceived() {
        // The receiver joins a stream that already started: the first frame it
        // sees defines the origin, so no silence is inserted at the beginning.
        val buffer = JitterBuffer(frameBytes)
        buffer.push(1, frame(2))
        val out = ByteArray(frameBytes)
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(2.toByte(), out[0])
        assertEquals(0, buffer.filledSilences)
    }

    @Test
    fun aMissingFrameIsFilledWithSilenceOnceEnoughFramesWait() {
        val buffer = JitterBuffer(frameBytes, capacityFrames = 12, gapThresholdFrames = 3)
        buffer.push(0, frame(1))
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(ByteArray(frameBytes)))
        // Frame 1 is lost; 2, 3 and 4 arrive behind it.
        buffer.push(2, frame(3))
        buffer.push(3, frame(4))
        buffer.push(4, frame(5))
        val out = ByteArray(frameBytes)
        // The gap is filled with silence instead of stalling the playback clock.
        assertEquals(JitterBuffer.Pull.SILENCE, buffer.pull(out))
        assertTrue(out.all { it == 0.toByte() })
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(3.toByte(), out[0])
        assertEquals(1, buffer.filledSilences)
    }

    @Test
    fun aMissingFrameIsWaitedForWhileTheBufferIsStillShallow() {
        val buffer = JitterBuffer(frameBytes, capacityFrames = 12, gapThresholdFrames = 3)
        buffer.push(0, frame(1))
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(ByteArray(frameBytes)))
        // Frame 1 is missing and only two newer frames are waiting: it still has
        // a chance to arrive, so nothing is played yet.
        buffer.push(2, frame(3))
        buffer.push(3, frame(4))
        assertEquals(JitterBuffer.Pull.EMPTY, buffer.pull(ByteArray(frameBytes)))
        assertEquals(0, buffer.filledSilences)
        // A third newer frame makes the wait pointless: silence is inserted.
        buffer.push(4, frame(5))
        assertEquals(JitterBuffer.Pull.SILENCE, buffer.pull(ByteArray(frameBytes)))
        assertEquals(1, buffer.filledSilences)
    }

    @Test
    fun aFrameArrivingAfterItsSlotIsDroppedNotPlayed() {
        val buffer = JitterBuffer(frameBytes, capacityFrames = 12, gapThresholdFrames = 1)
        buffer.push(0, frame(1))
        buffer.push(1, frame(2))
        val out = ByteArray(frameBytes)
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(out))
        // Frame 0 arrives again, far too late.
        buffer.push(0, frame(9))
        assertEquals(1, buffer.droppedFrames)
        assertEquals(JitterBuffer.Pull.EMPTY, buffer.pull(out))
    }

    @Test
    fun duplicatesAreIgnored() {
        val buffer = JitterBuffer(frameBytes)
        buffer.push(0, frame(1))
        buffer.push(0, frame(1))
        assertEquals(1, buffer.size)
        assertEquals(JitterBuffer.Pull.FRAME, buffer.pull(ByteArray(frameBytes)))
        assertEquals(JitterBuffer.Pull.EMPTY, buffer.pull(ByteArray(frameBytes)))
    }

    @Test
    fun theBufferIsBoundedSoLatencyCannotGrow() {
        val capacity = 4
        val buffer = JitterBuffer(frameBytes, capacityFrames = capacity, gapThresholdFrames = 1)
        for (index in 0 until 20) {
            buffer.push(index, frame(index))
        }
        assertTrue(buffer.size <= capacity)
        assertTrue(buffer.droppedFrames > 0)
    }

    @Test
    fun aFrameOfTheWrongSizeIsIgnored() {
        val buffer = JitterBuffer(frameBytes)
        buffer.push(0, ByteArray(frameBytes - 1))
        assertEquals(0, buffer.size)
        buffer.push(0, ByteArray(frameBytes + 1))
        assertEquals(0, buffer.size)
    }

    @Test
    fun resetClearsEverything() {
        val buffer = JitterBuffer(frameBytes)
        buffer.push(5, frame(1))
        buffer.reset()
        assertEquals(0, buffer.size)
        assertEquals(-1, buffer.expectedSequence)
        assertEquals(JitterBuffer.Pull.EMPTY, buffer.pull(ByteArray(frameBytes)))
    }
}
