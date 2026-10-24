package com.jpo.cellnet.text

import com.jpo.cellnet.data.BlockSet
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Long text fragmentation: the chunking must be lossless, must never cut a
 * multi byte character in the middle, and the reassembly must ignore duplicates
 * and tolerate out of order arrival.
 */
class TextChunkerTest {

    @Test
    fun shortMessageDoesNotNeedFragmentation() {
        assertFalse(TextChunker.needsFragmentation("bonjour"))
        assertEquals(1, TextChunker.chunks("bonjour").size)
    }

    @Test
    fun longMessageIsSplitInBoundedChunks() {
        val body = "a".repeat(TextChunker.CHUNK_SIZE * 4 + 7)
        val chunks = TextChunker.chunks(body)
        assertEquals(5, chunks.size)
        chunks.forEach { chunk ->
            assertTrue(chunk.data.size <= TextChunker.CHUNK_SIZE)
            assertEquals(chunks.size, chunk.total)
        }
        // Every index appears exactly once and in order.
        assertEquals((0 until chunks.size).toList(), chunks.map { it.index })
    }

    @Test
    fun chunkingNeverCutsAMultiByteCharacter() {
        // 3 bytes per character: a naive byte split would produce invalid UTF-8.
        val body = "\u20AC".repeat(TextChunker.CHUNK_SIZE)  // 768 bytes
        val chunks = TextChunker.chunks(body)
        assertTrue(chunks.size > 1)
        val rebuilt = chunks.sortedBy { it.index }
            .joinToString("") { String(it.data, Charsets.UTF_8) }
        assertEquals(body, rebuilt)
    }

    @Test
    fun reassemblyIsLosslessForALongUnicodeMessage() {
        val body = buildString {
            repeat(400) { append("message $it éàçü 漢字 ") }
        }
        val chunks = TextChunker.chunks(body)
        assertTrue(chunks.size > 1)

        val assembler = TextAssembler()
        assembler.begin("m-0123456789ab", chunks.size)
        // Arrival in reverse order on purpose.
        for (chunk in chunks.reversed()) {
            assertTrue(assembler.add("m-0123456789ab", chunk.index, chunk.total, chunk.data))
        }
        assertTrue(assembler.isComplete("m-0123456789ab"))
        assertEquals(body, assembler.assemble("m-0123456789ab"))
    }

    @Test
    fun duplicateChunkIsIgnoredAndNeverCorruptsTheResult() {
        val body = "x".repeat(TextChunker.CHUNK_SIZE * 2)
        val chunks = TextChunker.chunks(body)
        val assembler = TextAssembler()
        assembler.begin("m-1", chunks.size)
        assertTrue(assembler.add("m-1", 0, chunks.size, chunks[0].data))
        assertFalse(assembler.add("m-1", 0, chunks.size, chunks[0].data))  // duplicate
        assertTrue(assembler.add("m-1", 1, chunks.size, chunks[1].data))
        assertEquals(body, assembler.assemble("m-1"))
    }

    @Test
    fun incompleteTransferIsNeverAssembledAndListsWhatIsMissing() {
        val body = "y".repeat(TextChunker.CHUNK_SIZE * 3)
        val chunks = TextChunker.chunks(body)
        val assembler = TextAssembler()
        assembler.begin("m-2", chunks.size)
        assembler.add("m-2", 0, chunks.size, chunks[0].data)
        assembler.add("m-2", 2, chunks.size, chunks[2].data)
        assertFalse(assembler.isComplete("m-2"))
        assertNull(assembler.assemble("m-2"))
        assertEquals(listOf(1), assembler.missing("m-2"))
    }

    @Test
    fun blockReassemblyTracksProgressAndSerializesIt() {
        val plan = BlockReassembly(5)
        assertTrue(plan.accept(4))
        assertTrue(plan.accept(0))
        assertFalse(plan.accept(4))          // idempotent
        assertFalse(plan.accept(9))          // out of range
        assertFalse(plan.isComplete())
        assertEquals(listOf(1, 2, 3), plan.missing())
        assertEquals(2, plan.receivedCount())
        assertEquals("0,4", plan.encode())

        val restored = BlockReassembly(5)
        restored.restore(plan.encode())
        assertEquals(listOf(1, 2, 3), restored.missing())
        assertTrue(restored.accept(1))
        assertTrue(restored.accept(2))
        assertTrue(restored.accept(3))
        assertTrue(restored.isComplete())
    }

    @Test
    fun blockSetCsvRoundTripIgnoresGarbage() {
        assertEquals("0,1,5", BlockSet.encode(setOf(5, 0, 1)))
        assertEquals(setOf(0, 1, 5), BlockSet.decode("0,1,5"))
        assertEquals(setOf(0, 1, 5), BlockSet.decode(" 5 , 0 , 1 "))
        assertEquals(emptySet<Int>(), BlockSet.decode(""))
        assertEquals(emptySet<Int>(), BlockSet.decode("abc,-3"))
        assertEquals(setOf(2), BlockSet.decode("2,2,2"))
    }
}
