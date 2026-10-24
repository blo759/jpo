package com.jpo.cellnet.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * Voice note reassembly, resumption and integrity checks. The directory is a
 * temporary folder, so the whole class runs on the JVM without Android.
 */
class VoiceAssemblerTest {

    @get:Rule
    val folder = TemporaryFolder()

    private val msgId = "m-0123456789ab"

    private fun payload(size: Int, seed: Int): ByteArray =
        ByteArray(size) { ((it + seed) % 251).toByte() }

    @Test
    fun outOfOrderBlocksAreReassembledAndValidated() {
        val bytes = payload(1000, 0)
        val total = VoiceProtocol.blockCount(bytes.size.toLong())
        assertEquals(3, total)
        val assembler = VoiceAssembler(folder.root)

        val expectedCrc = VoiceProtocol.crc32(bytes)
        assembler.begin(msgId, total, bytes.size.toLong(), expectedCrc)
        // Reverse order on purpose.
        for (index in (0 until total).reversed()) {
            val start = index * VoiceProtocol.BLOCK_SIZE
            val end = minOf(bytes.size, start + VoiceProtocol.BLOCK_SIZE)
            assertNotNull(assembler.add(msgId, index, bytes.copyOfRange(start, end)))
        }
        assertTrue(assembler.missing(msgId).isEmpty())

        val result = assembler.finish(msgId)
        assertTrue(result.ok)
        val file = result.file
        assertNotNull(file)
        assertTrue(file!!.exists())
        assertEquals(bytes.size.toLong(), file.length())
        assertTrue(file.readBytes().contentEquals(bytes))
    }

    @Test
    fun duplicateBlockIsIgnored() {
        val bytes = payload(500, 7)
        val total = VoiceProtocol.blockCount(bytes.size.toLong())
        val assembler = VoiceAssembler(folder.root)
        assembler.begin(msgId, total, bytes.size.toLong(), VoiceProtocol.crc32(bytes))

        val first = bytes.copyOfRange(0, VoiceProtocol.BLOCK_SIZE)
        assertEquals(1, assembler.add(msgId, 0, first)?.received)
        assertEquals(1, assembler.add(msgId, 0, first)?.received)
        assertTrue(assembler.receivedIndexes(msgId).contains(0))
    }

    @Test
    fun wrongCrcNeverPublishesTheFile() {
        val bytes = payload(600, 3)
        val total = VoiceProtocol.blockCount(bytes.size.toLong())
        val assembler = VoiceAssembler(folder.root)
        assembler.begin(msgId, total, bytes.size.toLong(), 12345L)  // wrong CRC
        for (index in 0 until total) {
            val start = index * VoiceProtocol.BLOCK_SIZE
            val end = minOf(bytes.size, start + VoiceProtocol.BLOCK_SIZE)
            assembler.add(msgId, index, bytes.copyOfRange(start, end))
        }
        val result = assembler.finish(msgId)
        assertFalse(result.ok)
        assertTrue(result.error?.contains("CRC") == true)
        assertFalse(File(folder.root, "$msgId.m4a").exists())
        assertFalse(File(folder.root, "$msgId.part").exists())
    }

    @Test
    fun interruptedTransferIsResumedFromThePersistedBlocks() {
        val bytes = payload(1200, 11)
        val total = VoiceProtocol.blockCount(bytes.size.toLong())
        val crc = VoiceProtocol.crc32(bytes)

        // First attempt: only the first two blocks arrive.
        val first = VoiceAssembler(folder.root)
        first.begin(msgId, total, bytes.size.toLong(), crc)
        for (index in 0 until 2) {
            val start = index * VoiceProtocol.BLOCK_SIZE
            first.add(msgId, index, bytes.copyOfRange(start, start + VoiceProtocol.BLOCK_SIZE))
        }
        val persisted = first.receivedIndexes(msgId)
        assertEquals(listOf(0, 1), persisted)
        // The application restarts: the previous instance disappears (its file
        // handle is closed) but the ".part" file and the block list stay on disk.
        first.discard(msgId)
        assertTrue(File(folder.root, "$msgId.part").exists())

        // Application restart: the state is rebuilt from the persisted indexes and
        // only the missing blocks are requested.
        val resumed = VoiceAssembler(folder.root)
        resumed.restore(msgId, total, bytes.size.toLong(), crc, persisted)
        assertEquals(listOf(2), resumed.missing(msgId))
        val start = 2 * VoiceProtocol.BLOCK_SIZE
        val end = minOf(bytes.size, start + VoiceProtocol.BLOCK_SIZE)
        resumed.add(msgId, 2, bytes.copyOfRange(start, end))

        val result = resumed.finish(msgId)
        assertTrue(result.ok)
        assertEquals(bytes.size.toLong(), result.file?.length())
    }

    @Test
    fun expiredPartialFilesAreRemoved() {
        val orphan = File(folder.root, "m-ffffffffffff.part")
        orphan.writeBytes(ByteArray(16))
        orphan.setLastModified(System.currentTimeMillis() - 48L * 3600L * 1000L)
        val recent = File(folder.root, "m-eeeeeeeeeeee.part")
        recent.writeBytes(ByteArray(16))

        val assembler = VoiceAssembler(folder.root)
        val removed = assembler.discardExpired(24L * 3600L * 1000L, System.currentTimeMillis())
        assertEquals(1, removed)
        assertFalse(orphan.exists())
        assertTrue(recent.exists())
    }
}
