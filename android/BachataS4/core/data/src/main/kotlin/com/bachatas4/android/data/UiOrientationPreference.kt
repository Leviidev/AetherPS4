package com.bachatas4.android.data

import android.content.Context
import android.content.pm.ActivityInfo

enum class UiOrientation {
    Portrait,
    Landscape,
}

object UiOrientationPreference {
    const val FILE_NAME = "ui_preferences"
    const val KEY = "ui_orientation"
    val DEFAULT = UiOrientation.Portrait

    fun decode(value: String?): UiOrientation = when (value?.trim()) {
        "portrait" -> UiOrientation.Portrait
        "landscape" -> UiOrientation.Landscape
        else -> DEFAULT
    }

    fun encode(value: UiOrientation): String = when (value) {
        UiOrientation.Portrait -> "portrait"
        UiOrientation.Landscape -> "landscape"
    }

    fun toActivityOrientation(value: UiOrientation): Int = when (value) {
        UiOrientation.Portrait -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT
        UiOrientation.Landscape -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    }

    fun toggle(value: UiOrientation): UiOrientation = when (value) {
        UiOrientation.Portrait -> UiOrientation.Landscape
        UiOrientation.Landscape -> UiOrientation.Portrait
    }

    fun read(context: Context): UiOrientation {
        val prefs = context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
        return decode(prefs.getString(KEY, null))
    }

    fun write(context: Context, value: UiOrientation) {
        context.getSharedPreferences(FILE_NAME, Context.MODE_PRIVATE)
            .edit()
            .putString(KEY, encode(value))
            .apply()
    }
}
