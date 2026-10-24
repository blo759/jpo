package com.jpo.cellnet.data

import androidx.room.Dao
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query

/**
 * Durable progress of a resumable transfer.
 *
 * A voice note (500 blocks for 60 s) or a long text (up to 128 chunks) is never
 * restarted from scratch: the set of blocks already validated is stored here, so
 * after a network loss, a cell change or an application restart only the missing
 * blocks are requested again (`voice_nack` / `text_nack`).
 *
 * `received` is a compact CSV of block indexes ("0,1,2,5") which keeps the row
 * small and readable, and makes the merge idempotent.
 */
@Entity(tableName = "transfers")
data class TransferEntity(
    @PrimaryKey val id: String,
    val kind: String,          // "voice" | "text"
    val direction: String,     // "out" | "in"
    val peer: String,
    val total: Int,
    val bytes: Long,
    val crc: Long,
    val received: String,      // CSV of the block indexes already validated
    val missing: String,       // CSV of the blocks the peer asked for again ("")
    val state: String,         // "active" | "complete" | "abandoned"
    val updatedAt: Long
)

/** Lifecycle values of [TransferEntity.state]. */
object TransferState {
    const val ACTIVE = "active"
    const val COMPLETE = "complete"
    const val ABANDONED = "abandoned"
}

@Dao
interface TransferDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsert(transfer: TransferEntity)

    @Query("SELECT * FROM transfers WHERE id = :id LIMIT 1")
    suspend fun find(id: String): TransferEntity?

    @Query("SELECT * FROM transfers WHERE state = 'active' ORDER BY updatedAt ASC")
    suspend fun active(): List<TransferEntity>

    @Query("SELECT * FROM transfers WHERE state = 'active' AND direction = 'in' ORDER BY updatedAt ASC")
    suspend fun activeIncoming(): List<TransferEntity>

    @Query("SELECT * FROM transfers WHERE state = 'active' AND direction = 'out' ORDER BY updatedAt ASC")
    suspend fun activeOutgoing(): List<TransferEntity>

    @Query("UPDATE transfers SET received = :received, updatedAt = :at WHERE id = :id")
    suspend fun setReceived(id: String, received: String, at: Long)

    @Query("UPDATE transfers SET missing = :missing, updatedAt = :at WHERE id = :id")
    suspend fun setMissing(id: String, missing: String, at: Long)

    @Query("UPDATE transfers SET state = :state, updatedAt = :at WHERE id = :id")
    suspend fun setState(id: String, state: String, at: Long)

    @Query("DELETE FROM transfers WHERE id = :id")
    suspend fun delete(id: String)

    @Query("DELETE FROM transfers WHERE state = 'complete' AND updatedAt < :before")
    suspend fun purgeCompleted(before: Long)

    @Query("DELETE FROM transfers WHERE state = 'active' AND updatedAt < :before")
    suspend fun purgeStale(before: Long)
}

/**
 * CSV encoding of a set of block indexes. Pure Kotlin on purpose: it is unit
 * tested on the JVM without any Android dependency.
 */
object BlockSet {
    fun encode(blocks: Collection<Int>): String =
        blocks.asSequence().sorted().joinToString(",")

    fun decode(text: String?): MutableSet<Int> {
        if (text.isNullOrBlank()) return mutableSetOf()
        return text.split(',')
            .mapNotNull { it.trim().toIntOrNull() }
            .filter { it >= 0 }
            .toMutableSet()
    }
}
