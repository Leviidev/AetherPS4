package com.bachatas4.android.database

import androidx.room.Dao
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query

@Entity(tableName = "sessions")
data class SessionEntity(
    @PrimaryKey val id: String,
    val gameId: String,
    val state: String,
    val startedAtMs: Long,
    val stoppedAtMs: Long?,
    val exitCode: Int?,
)

@Dao
interface SessionDao {
    @Insert(onConflict = OnConflictStrategy.ABORT)
    suspend fun insert(session: SessionEntity)

    @Query("SELECT * FROM sessions WHERE state = 'RUNNING'")
    suspend fun runningSessions(): List<SessionEntity>

    @Query(
        """
        UPDATE sessions
        SET state = 'STOPPED', stoppedAtMs = :stoppedAtMs, exitCode = :exitCode
        WHERE state = 'RUNNING'
        """,
    )
    suspend fun recoverStaleRunningSessions(
        stoppedAtMs: Long,
        exitCode: Int,
    ): Int
}
