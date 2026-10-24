package com.jpo.cellnet.data

import androidx.room.Dao
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query
import kotlinx.coroutines.flow.Flow

/**
 * Local journal of a live call.
 *
 * A call carries no audio content in the database: the media is relayed live by
 * the local station and is never recorded. Only the metadata (who, when, how
 * long, how it ended) is kept, so the conversation screen can show "appel de
 * 1 min 12" the way it shows a text or a voice note.
 *
 * The table is added by a purely additive migration: conversations, messages,
 * transfers and the user identity are untouched.
 */
@Entity(tableName = "calls")
data class CallEntity(
    @PrimaryKey val id: String,          // call id, 8 characters
    val peerId: String,
    val peerName: String,
    val cell: String?,                   // cell on which the call happened
    val direction: String,               // "out" | "in"
    val state: String,                   // CallRecordState
    val startedAt: Long,
    val connectedAt: Long? = null,
    val endedAt: Long? = null,
    val durationMs: Long = 0L,
    val reason: String? = null
)

/** Lifecycle values of [CallEntity.state]. */
object CallRecordState {
    const val DIALING = "dialing"
    const val RINGING = "ringing"
    const val ACTIVE = "active"
    const val ENDED = "ended"
    const val MISSED = "missed"
    const val REFUSED = "refused"
    const val FAILED = "failed"
}

@Dao
interface CallDao {
    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsert(call: CallEntity)

    @Query("SELECT * FROM calls WHERE id = :id LIMIT 1")
    suspend fun find(id: String): CallEntity?

    @Query("SELECT * FROM calls ORDER BY startedAt DESC LIMIT :limit")
    fun observeRecent(limit: Int = 100): Flow<List<CallEntity>>

    @Query("SELECT * FROM calls WHERE state IN ('dialing','ringing','active')")
    suspend fun active(): List<CallEntity>

    @Query("UPDATE calls SET state = :state, reason = :reason, endedAt = :endedAt, durationMs = :durationMs WHERE id = :id")
    suspend fun close(id: String, state: String, reason: String?, endedAt: Long, durationMs: Long)

    @Query("DELETE FROM calls WHERE state = 'ended' AND endedAt < :before")
    suspend fun purgeOlderThan(before: Long)
}
