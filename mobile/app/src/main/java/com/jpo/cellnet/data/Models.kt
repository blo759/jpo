package com.jpo.cellnet.data

import android.content.Context
import androidx.room.Dao
import androidx.room.Database
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query
import androidx.room.Room
import androidx.room.RoomDatabase
import androidx.room.Update
import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase
import kotlinx.coroutines.flow.Flow

/** Message states shared by the UI, the transport and the persistence layer. */
object MessageState {
    const val PENDING = "pending"        // created locally, not yet accepted by the station
    const val SENDING = "sending"        // HTTP upload in progress
    const val SENT = "sent"              // accepted by the local station, no receipt yet
    const val RECEIVING = "receiving"    // incoming voice being reassembled
    const val RECEIVED = "received"      // incoming message stored on this phone
    const val DELIVERED = "delivered"    // receiver stored it and confirmed
    const val READ = "read"              // receiver opened / listened to it
    const val FAILED = "failed"          // retries exhausted
}

@Entity(tableName = "conversations")
data class ConversationEntity(
    @PrimaryKey val peerId: String,
    val peerName: String,
    val station: String? = null,
    val lastMessageAt: Long = 0L,
    val lastPreview: String = "",
    val unread: Int = 0,
    val lastMessageId: String? = null
)

@Entity(tableName = "messages")
data class MessageEntity(
    @PrimaryKey val id: String,
    val conversationId: String,
    val fromUser: String,
    val toUser: String,
    val kind: String,
    val body: String = "",
    val createdAt: Long,
    val state: String,
    val isMine: Boolean,
    val voicePath: String? = null,
    val voiceDurationMs: Long = 0L,
    val voiceBytes: Long = 0L,
    val voiceTotalBlocks: Int = 0,
    val voiceReceivedBlocks: Int = 0,
    val voiceCrc: Long = 0L,
    val voicePlayedAt: Long? = null,
    val error: String? = null
)

@Dao
interface ChatDao {
    @Query("SELECT * FROM conversations ORDER BY lastMessageAt DESC")
    fun observeConversations(): Flow<List<ConversationEntity>>

    @Query("SELECT * FROM messages WHERE conversationId = :peerId ORDER BY createdAt ASC, id ASC")
    fun observeMessages(peerId: String): Flow<List<MessageEntity>>

    @Query("SELECT * FROM messages WHERE id = :id LIMIT 1")
    suspend fun findMessage(id: String): MessageEntity?

    @Insert(onConflict = OnConflictStrategy.IGNORE)
    suspend fun insertMessage(message: MessageEntity): Long

    @Update
    suspend fun updateMessage(message: MessageEntity)

    @Query("UPDATE messages SET state = :state WHERE id = :id")
    suspend fun setState(id: String, state: String)

    @Query("UPDATE messages SET state = :state, error = :error WHERE id = :id")
    suspend fun setStateWithError(id: String, state: String, error: String?)

    @Query("UPDATE messages SET voiceReceivedBlocks = :received WHERE id = :id")
    suspend fun setReceivedBlocks(id: String, received: Int)

    @Query("UPDATE messages SET voicePath = :path, state = :state, error = NULL, voiceReceivedBlocks = voiceTotalBlocks WHERE id = :id")
    suspend fun completeVoice(id: String, path: String, state: String)

    @Query("UPDATE messages SET voicePlayedAt = :at WHERE id = :id")
    suspend fun setPlayed(id: String, at: Long)

    @Insert(onConflict = OnConflictStrategy.REPLACE)
    suspend fun upsertConversation(conversation: ConversationEntity)

    @Query("SELECT * FROM conversations WHERE peerId = :peerId LIMIT 1")
    suspend fun findConversation(peerId: String): ConversationEntity?

    @Query("UPDATE conversations SET unread = 0 WHERE peerId = :peerId")
    suspend fun clearUnread(peerId: String)

    @Query("UPDATE conversations SET station = :station WHERE peerId = :peerId")
    suspend fun setConversationStation(peerId: String, station: String)

    @Query("SELECT * FROM messages WHERE isMine = 1 AND state IN ('pending','sending','failed') ORDER BY createdAt ASC")
    suspend fun outgoingPending(): List<MessageEntity>

    @Query("SELECT * FROM messages WHERE isMine = 0 AND conversationId = :peerId AND state != 'read'")
    suspend fun unreadIncoming(peerId: String): List<MessageEntity>

    @Query("SELECT * FROM messages WHERE state = 'receiving'")
    suspend fun receivingMessages(): List<MessageEntity>

    @Query("DELETE FROM messages WHERE id = :id")
    suspend fun deleteMessage(id: String)

    @Query("DELETE FROM messages WHERE state = 'receiving'")
    suspend fun purgeIncompleteIncoming()
}

@Database(
    entities = [ConversationEntity::class, MessageEntity::class, TransferEntity::class],
    version = 2,
    exportSchema = false
)
abstract class AppDatabase : RoomDatabase() {
    abstract fun chatDao(): ChatDao
    abstract fun transferDao(): TransferDao

    companion object {
        @Volatile
        private var instance: AppDatabase? = null

        /**
         * Adds the resumable transfer table without touching the existing
         * conversations and messages: upgrading the application must never lose
         * the history nor the identity of the user.
         */
        private val MIGRATION_1_2 = object : Migration(1, 2) {
            override fun migrate(db: SupportSQLiteDatabase) {
                db.execSQL(
                    "CREATE TABLE IF NOT EXISTS `transfers` (" +
                        "`id` TEXT NOT NULL, " +
                        "`kind` TEXT NOT NULL, " +
                        "`direction` TEXT NOT NULL, " +
                        "`peer` TEXT NOT NULL, " +
                        "`total` INTEGER NOT NULL, " +
                        "`bytes` INTEGER NOT NULL, " +
                        "`crc` INTEGER NOT NULL, " +
                        "`received` TEXT NOT NULL, " +
                        "`missing` TEXT NOT NULL, " +
                        "`state` TEXT NOT NULL, " +
                        "`updatedAt` INTEGER NOT NULL, " +
                        "PRIMARY KEY(`id`))"
                )
            }
        }

        fun get(context: Context): AppDatabase =
            instance ?: synchronized(this) {
                instance ?: Room.databaseBuilder(
                    context.applicationContext,
                    AppDatabase::class.java,
                    "cellnet.db"
                ).addMigrations(MIGRATION_1_2)
                    .build()
                    .also { instance = it }
            }
    }
}
