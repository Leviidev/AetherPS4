package com.bachatas4.android.database

import androidx.room.Dao
import androidx.room.Entity
import androidx.room.Insert
import androidx.room.OnConflictStrategy
import androidx.room.PrimaryKey
import androidx.room.Query
import kotlinx.coroutines.flow.Flow

@Entity(tableName = "games")
data class GameEntity(
    @PrimaryKey val id: String,
    val title: String,
    val relativePath: String,
    val sourceUri: String,
    val importedAtMs: Long,
    val subtitle: String? = null,
    val detail: String? = null,
    val lastLaunchedAtMs: Long = 0L,
)

@Dao
interface GameDao {
    @Insert(onConflict = OnConflictStrategy.ABORT)
    suspend fun insert(game: GameEntity)

    @Query("SELECT * FROM games ORDER BY title COLLATE NOCASE")
    fun observeAll(): Flow<List<GameEntity>>

    @Query("SELECT * FROM games")
    suspend fun getAll(): List<GameEntity>

    @Query("SELECT * FROM games WHERE id = :id")
    suspend fun getById(id: String): GameEntity?

    @Query("UPDATE games SET title = :title WHERE id = :id")
    suspend fun updateTitle(id: String, title: String): Int

    @Query(
        "UPDATE games SET title = :title, subtitle = :subtitle, detail = :detail, " +
            "importedAtMs = :importedAtMs WHERE id = :id",
    )
    suspend fun updateMetadata(
        id: String,
        title: String,
        subtitle: String?,
        detail: String?,
        importedAtMs: Long,
    ): Int

    @Query("UPDATE games SET lastLaunchedAtMs = :timestamp WHERE id = :id")
    suspend fun updateLastLaunched(id: String, timestamp: Long): Int

    @Query("DELETE FROM games WHERE id = :id")
    suspend fun deleteById(id: String): Int
}
