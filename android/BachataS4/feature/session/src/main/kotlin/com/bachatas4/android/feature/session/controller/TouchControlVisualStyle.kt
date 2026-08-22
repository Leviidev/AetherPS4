package com.bachatas4.android.feature.session.controller

enum class TouchControlVisualStyle {
    Face,
    Dpad,
    Stick,
    Shoulder,
    Trigger,
    Touchpad,
    Center,
    Unknown,
    ;

    companion object {
        fun forControl(control: String): TouchControlVisualStyle = when (control) {
            "triangle", "circle", "cross", "square" -> Face
            "dpad_up", "dpad_right", "dpad_down", "dpad_left" -> Dpad
            "left_stick", "right_stick" -> Stick
            "l1", "r1" -> Shoulder
            "l2", "r2" -> Trigger
            "touchpad" -> Touchpad
            "options", "share", "ps" -> Center
            else -> Unknown
        }
    }
}
