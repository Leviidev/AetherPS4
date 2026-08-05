package com.bachatas4.android.feature.session

import android.content.pm.ActivityInfo

enum class SessionWindowMode(
    val orientation: Int,
    val hideSystemBars: Boolean,
) {
    /** Default non-session axis lock (also UiOrientationPreference default). */
    Portrait(ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT, false),
    ImmersiveLandscape(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE, true),
}
