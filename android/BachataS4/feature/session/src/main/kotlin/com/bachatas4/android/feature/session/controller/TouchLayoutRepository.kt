package com.bachatas4.android.feature.session.controller

import android.content.Context
import dagger.hilt.android.qualifiers.ApplicationContext
import java.io.File
import javax.inject.Inject
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

class TouchLayoutRepository @Inject constructor(@ApplicationContext context: Context) {
    private val root = File(context.filesDir, "settings/touch-layouts")
    private val json = Json { prettyPrint = true; ignoreUnknownKeys = false }

    fun load(id: String?): TouchLayout {
        if (id == null || id == "default") return TouchLayout()
        require(id.matches(Regex("[A-Za-z0-9._-]+"))) { "Invalid touch layout id" }
        val file = File(root, "$id.json")
        return if (file.isFile) json.decodeFromString<TouchLayout>(file.readText()) else TouchLayout()
    }

    fun save(layout: TouchLayout) {
        root.mkdirs()
        val file = File(root, "${layout.id}.json")
        val temporary = File(root, "${layout.id}.json.tmp")
        temporary.writeText(json.encodeToString(layout))
        require(temporary.renameTo(file) || run { temporary.copyTo(file, overwrite = true); temporary.delete(); true })
    }
}
