package com.jpo.cellnet.text

import com.jpo.cellnet.data.BlockSet

/**
 * Application level fragmentation of long text messages.
 *
 * A text longer than [CHUNK_SIZE] is split into self contained chunks, each sent
 * as its own `text_block` payload. The station relays them verbatim (it does not
 * parse the JSON body), so:
 *   * a chunk always fits in a single bus frame (no bus fragmentation needed),
 *   * a lost chunk is retransmitted alone (`text_nack` lists the missing ones),
 *   * the transfer can resume after a reconnection or an application restart.
 *
 * Pure Kotlin: unit tested on the JVM without Android.
 */
object TextChunker {

    /** 256 UTF-8 bytes keep a chunk inside one 480 byte bus frame with margin. */
    const val CHUNK_SIZE = 256

    /** Explicit application limit: 128 chunks, refused with a clear error above. */
    const val MAX_TEXT_BYTES = CHUNK_SIZE * 128

    data class Chunk(val index: Int, val total: Int, val data: ByteArray) {
        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is Chunk) return false
            return index == other.index && total == other.total && data.contentEquals(other.data)
        }

        override fun hashCode(): Int {
            var result = index
            result = 31 * result + total
            result = 31 * result + data.contentHashCode()
            return result
        }
    }

    /** True when the message needs the chunked path instead of a single frame. */
    fun needsFragmentation(body: String): Boolean =
        body.toByteArray(Charsets.UTF_8).size > CHUNK_SIZE

    /**
     * Splits on UTF-8 boundaries: a chunk never cuts a multi byte character in
     * the middle, which is what makes the reassembled text identical to the
     * original.
     */
    fun chunks(body: String): List<Chunk> {
        val bytes = body.toByteArray(Charsets.UTF_8)
        if (bytes.size <= CHUNK_SIZE) {
            return listOf(Chunk(0, 1, bytes))
        }
        val pieces = ArrayList<ByteArray>()
        var start = 0
        while (start < bytes.size) {
            var end = minOf(bytes.size, start + CHUNK_SIZE)
            // 10xxxxxx is a UTF-8 continuation byte: never start a chunk on one.
            while (end > start && end < bytes.size && (bytes[end].toInt() and 0xC0) == 0x80) {
                end--
            }
            if (end == start) {
                end = minOf(bytes.size, start + CHUNK_SIZE)
            }
            pieces.add(bytes.copyOfRange(start, end))
            start = end
        }
        val total = pieces.size
        return pieces.mapIndexed { index, data -> Chunk(index, total, data) }
    }
}

/**
 * Received block bookkeeping for one application level transfer.
 *
 * The received set is what makes the transfer resumable: it can be serialized
 * into the `transfers` table, restored after a restart and turned into the list
 * of blocks to ask for again. Accepting a block twice is a no-op, so a duplicated
 * or retransmitted chunk never corrupts the result.
 */
class BlockReassembly(private val total: Int) {

    private val received = sortedSetOf<Int>()

    fun accept(index: Int): Boolean {
        if (index < 0 || index >= total) return false
        return received.add(index)
    }

    fun missing(): List<Int> = (0 until total).filter { !received.contains(it) }

    fun isComplete(): Boolean = received.size >= total && total > 0

    fun receivedCount(): Int = received.size

    fun receivedIndexes(): List<Int> = received.toList()

    fun encode(): String = BlockSet.encode(received)

    fun restore(encoded: String?) {
        received.clear()
        received.addAll(BlockSet.decode(encoded).filter { it < total })
    }
}

/**
 * Reassembles the chunks of a long text, in memory only: the durable progress
 * lives in the `transfers` table, this class only holds the payload bytes of the
 * transfer currently in flight.
 */
class TextAssembler {

    private class Session(val total: Int) {
        val blocks = HashMap<Int, ByteArray>()
    }

    private val sessions = HashMap<String, Session>()

    fun begin(msgId: String, total: Int) {
        if (total <= 0) return
        sessions[msgId] = Session(total)
    }

    fun has(msgId: String): Boolean = sessions.containsKey(msgId)

    /** Returns true when the block was new (a duplicate returns false). */
    fun add(msgId: String, index: Int, total: Int, data: ByteArray): Boolean {
        val session = sessions[msgId] ?: Session(total).also { sessions[msgId] = it }
        if (index < 0 || index >= session.total) return false
        if (session.blocks.containsKey(index)) return false
        session.blocks[index] = data
        return true
    }

    fun isComplete(msgId: String): Boolean {
        val session = sessions[msgId] ?: return false
        if (session.blocks.size < session.total) return false
        return (0 until session.total).all { session.blocks.containsKey(it) }
    }

    fun missing(msgId: String): List<Int> {
        val session = sessions[msgId] ?: return emptyList()
        return (0 until session.total).filter { !session.blocks.containsKey(it) }
    }

    fun assemble(msgId: String): String? {
        if (!isComplete(msgId)) return null
        val session = sessions[msgId] ?: return null
        var size = 0
        for (index in 0 until session.total) {
            size += session.blocks[index]?.size ?: return null
        }
        val out = ByteArray(size)
        var offset = 0
        for (index in 0 until session.total) {
            val block = session.blocks[index] ?: return null
            System.arraycopy(block, 0, out, offset, block.size)
            offset += block.size
        }
        return String(out, Charsets.UTF_8)
    }

    fun discard(msgId: String) {
        sessions.remove(msgId)
    }
}
