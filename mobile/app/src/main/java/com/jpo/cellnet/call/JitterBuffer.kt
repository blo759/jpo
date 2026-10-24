package com.jpo.cellnet.call

import java.util.Arrays
import java.util.TreeMap

/**
 * Reordering and gap filling buffer of the incoming call stream.
 *
 * The local Wi-Fi link is fast but a frame can still be reordered or lost, and
 * the playback clock must never stall: a missing frame is replaced by silence as
 * soon as enough newer frames are waiting behind it, and a frame that arrives
 * after its slot was played is dropped instead of shifting the whole stream.
 *
 * The origin of the stream is the first frame actually received: the receiver
 * joins a stream that already started, so no silence is inserted at the very
 * beginning of a call.
 *
 * Pure Kotlin: unit tested on the JVM without Android.
 */
class JitterBuffer(
    private val frameBytes: Int = CallProtocol.FRAME_BYTES,
    private val capacityFrames: Int = CallProtocol.JITTER_MAX_FRAMES,
    private val gapThresholdFrames: Int = CallProtocol.GAP_THRESHOLD_FRAMES
) {

    enum class Pull {
        /** A real frame was written into the output buffer. */
        FRAME,

        /** A gap was detected and filled with silence. */
        SILENCE,

        /** Nothing to play right now (the stream has not started or is late). */
        EMPTY
    }

    private val pending = TreeMap<Int, ByteArray>()
    private var expected = -1
    private var dropped = 0
    private var filled = 0

    val size: Int get() = pending.size
    val expectedSequence: Int get() = expected
    val droppedFrames: Int get() = dropped
    val filledSilences: Int get() = filled

    fun reset() {
        pending.clear()
        expected = -1
        dropped = 0
        filled = 0
    }

    /** Queues one received frame. */
    fun push(sequence: Int, payload: ByteArray) {
        if (payload.size != frameBytes) return
        if (expected < 0) expected = sequence
        // Too late: its slot was already played (or silence filled it).
        if (sequence < expected) {
            dropped++
            return
        }
        if (pending.containsKey(sequence)) return
        pending[sequence] = payload
        // Bound the latency: the oldest frames are dropped first.
        while (pending.size > capacityFrames) {
            val oldest = pending.firstKey()
            pending.remove(oldest)
            if (oldest == expected) expected++
            dropped++
        }
    }

    /**
     * Writes the next playable frame into [out]. The caller decides what to do
     * with `EMPTY` (the audio engine writes silence to keep the track fed).
     */
    fun pull(out: ByteArray): Pull {
        if (out.size < frameBytes || expected < 0) return Pull.EMPTY
        val frame = pending.remove(expected)
        if (frame != null) {
            System.arraycopy(frame, 0, out, 0, frameBytes)
            expected++
            return Pull.FRAME
        }
        if (pending.isEmpty()) return Pull.EMPTY
        // A frame is missing. Wait a little for it, then fill with silence so the
        // playback clock keeps its cadence.
        if (pending.size >= gapThresholdFrames) {
            Arrays.fill(out, 0, frameBytes, 0)
            filled++
            expected++
            return Pull.SILENCE
        }
        return Pull.EMPTY
    }
}
