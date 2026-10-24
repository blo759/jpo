package com.jpo.cellnet.audio

import android.content.Context
import java.io.File
import java.io.RandomAccessFile

/**
 * Reassembles a voice note received block by block. Blocks are written at
 * `seq * BLOCK_SIZE` in a ".part" file, so an out of order arrival is harmless
 * and a duplicate block never corrupts the file. The complete file is only
 * published after the size and the CRC32 announced by the sender are verified.
 *
 * A transfer is resumable: [restore] reopens an interrupted transfer with the
 * blocks that were already validated (persisted by the caller), and
 * [receivedIndexes] gives the list to persist after every accepted block. The
 * directory is injected so the class can be unit tested on the JVM with a
 * temporary folder instead of an Android context.
 */
class VoiceAssembler(private val dir: File) {

    constructor(context: Context) : this(File(context.filesDir, "voice"))

    init {
        dir.mkdirs()
    }

    data class Progress(val received: Int, val total: Int)

    data class Result(
        val ok: Boolean,
        val file: File?,
        val missing: List<Int>,
        val received: Int,
        val total: Int,
        val error: String? = null
    )

    private class Session(
        val partFile: File,
        val total: Int,
        val expectedBytes: Long,
        val expectedCrc: Long
    ) {
        val received = sortedSetOf<Int>()
        val access = RandomAccessFile(partFile, "rw")
    }

    private val sessions = HashMap<String, Session>()

    fun partFile(msgId: String): File = File(dir, "$msgId.part")

    @Synchronized
    fun begin(msgId: String, total: Int, expectedBytes: Long, expectedCrc: Long): Progress {
        closeSession(msgId)
        val part = partFile(msgId)
        part.delete()
        sessions[msgId] = Session(part, total, expectedBytes, expectedCrc)
        return Progress(0, total)
    }

    /**
     * Reopens an interrupted transfer: the blocks listed in [receivedIndexes] are
     * considered valid and only the missing ones will be requested again.
     */
    @Synchronized
    fun restore(
        msgId: String,
        total: Int,
        expectedBytes: Long,
        expectedCrc: Long,
        receivedIndexes: Collection<Int>
    ): Progress {
        closeSession(msgId)
        val part = partFile(msgId)
        if (!part.exists()) {
            part.createNewFile()
        }
        val session = Session(part, total, expectedBytes, expectedCrc)
        session.received.addAll(receivedIndexes.filter { it >= 0 && it < total })
        sessions[msgId] = session
        return Progress(session.received.size, total)
    }

    @Synchronized
    fun add(msgId: String, seq: Int, data: ByteArray): Progress? {
        val session = sessions[msgId] ?: return null
        if (seq < 0 || seq >= session.total) return null
        if (!session.received.add(seq)) {
            return Progress(session.received.size, session.total)
        }
        session.access.seek(seq.toLong() * VoiceProtocol.BLOCK_SIZE)
        session.access.write(data)
        return Progress(session.received.size, session.total)
    }

    @Synchronized
    fun missing(msgId: String): List<Int> {
        val session = sessions[msgId] ?: return emptyList()
        return (0 until session.total).filter { !session.received.contains(it) }
    }

    @Synchronized
    fun receivedIndexes(msgId: String): List<Int> =
        sessions[msgId]?.received?.toList() ?: emptyList()

    @Synchronized
    fun has(msgId: String): Boolean = sessions.containsKey(msgId)

    @Synchronized
    fun finish(msgId: String): Result {
        val session = sessions[msgId]
            ?: return Result(false, null, emptyList(), 0, 0, "transfert inconnu")
        runCatching { session.access.close() }
        val part = session.partFile
        val missing = (0 until session.total).filter { !session.received.contains(it) }
        val received = session.received.size

        if (missing.isNotEmpty()) {
            sessions.remove(msgId)
            return Result(false, null, missing, received, session.total, "blocs manquants")
        }
        if (part.length() != session.expectedBytes) {
            part.delete()
            sessions.remove(msgId)
            return Result(false, null, emptyList(), received, session.total, "taille incorrecte")
        }
        if (VoiceProtocol.crc32(part) != session.expectedCrc) {
            part.delete()
            sessions.remove(msgId)
            return Result(false, null, emptyList(), received, session.total, "CRC32 invalide")
        }

        val finalFile = File(dir, "$msgId.m4a")
        finalFile.delete()
        var moved = part.renameTo(finalFile)
        if (!moved) {
            // The rename can be refused while a handle is still open on the part
            // file (observed on Windows). A recording that passed the size and CRC
            // checks must never be lost: fall back to a copy.
            moved = runCatching {
                part.inputStream().use { input ->
                    finalFile.outputStream().use { output -> input.copyTo(output) }
                }
                finalFile.length() == session.expectedBytes
            }.getOrDefault(false)
            if (moved) {
                part.delete()
            }
        }
        sessions.remove(msgId)
        return if (moved) {
            Result(true, finalFile, emptyList(), session.total, session.total)
        } else {
            Result(false, null, emptyList(), received, session.total, "écriture impossible")
        }
    }

    @Synchronized
    fun discard(msgId: String) {
        closeSession(msgId)
    }

    /**
     * Removes the partial files of transfers that were abandoned (no progress for
     * [maxAgeMs]). Called at start-up so an interrupted transfer that was never
     * resumed does not occupy the storage for ever.
     */
    @Synchronized
    fun discardExpired(maxAgeMs: Long, now: Long): Int {
        var removed = 0
        val known = sessions.keys.toSet()
        dir.listFiles()?.forEach { file ->
            if (!file.name.endsWith(".part")) return@forEach
            val msgId = file.name.removeSuffix(".part")
            if (known.contains(msgId)) return@forEach
            if (now - file.lastModified() > maxAgeMs) {
                if (file.delete()) removed++
            }
        }
        return removed
    }

    private fun closeSession(msgId: String) {
        sessions.remove(msgId)?.let { session ->
            runCatching { session.access.close() }
        }
    }
}
