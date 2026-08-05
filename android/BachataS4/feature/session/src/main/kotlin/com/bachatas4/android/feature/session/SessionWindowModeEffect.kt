package com.bachatas4.android.feature.session

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import com.bachatas4.android.data.UiOrientationPreference

@Composable
fun SessionWindowModeEffect() {
    val context = LocalContext.current
    val view = LocalView.current
    DisposableEffect(context, view) {
        val activity = context.findActivity()
        val controller = activity?.window?.let { WindowCompat.getInsetsController(it, view) }
        activity?.requestedOrientation = SessionWindowMode.ImmersiveLandscape.orientation
        controller?.let {
            it.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            if (SessionWindowMode.ImmersiveLandscape.hideSystemBars) {
                it.hide(WindowInsetsCompat.Type.systemBars())
            }
        }
        onDispose {
            val restored = UiOrientationPreference.read(context)
            activity?.requestedOrientation = UiOrientationPreference.toActivityOrientation(restored)
            controller?.show(WindowInsetsCompat.Type.systemBars())
        }
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.takeUnless { it === this }?.findActivity()
    else -> null
}
