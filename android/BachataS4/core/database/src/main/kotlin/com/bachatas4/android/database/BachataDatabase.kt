package com.bachatas4.android.database

import androidx.room.Database
import androidx.room.RoomDatabase

@Database(
    entities = [GameEntity::class, SessionEntity::class],
    version = 3,
    exportSchema = true,
)
abstract class BachataDatabase : RoomDatabase() {
    abstract fun gameDao(): GameDao
    abstract fun sessionDao(): SessionDao
}
