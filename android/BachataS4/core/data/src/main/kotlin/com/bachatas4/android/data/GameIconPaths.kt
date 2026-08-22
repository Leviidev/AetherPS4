package com.bachatas4.android.data

import java.io.File

object GameIconPaths {
    fun icon0(filesDir: File, relativePath: String): File =
        File(filesDir, "$relativePath/sce_sys/icon0.png")

    fun paramSfo(filesDir: File, relativePath: String): File =
        File(filesDir, "$relativePath/sce_sys/param.sfo")
}
