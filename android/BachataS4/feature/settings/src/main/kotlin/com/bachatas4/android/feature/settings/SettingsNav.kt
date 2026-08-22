package com.bachatas4.android.feature.settings

enum class SettingsNavIntent {
    PrevItem,
    NextItem,
    Open,
    Activate,
    LeaveOrParent,
    Ignore,
}

fun mapSettingsNav(
    focusArea: String,
    control: String,
    isLandscape: Boolean,
): SettingsNavIntent {
    return when (focusArea) {
        "tabs" -> when {
            isLandscape -> when (control) {
                "dpad_up" -> SettingsNavIntent.PrevItem
                "dpad_down" -> SettingsNavIntent.NextItem
                "dpad_right", "cross" -> SettingsNavIntent.Open
                "circle" -> SettingsNavIntent.LeaveOrParent
                else -> SettingsNavIntent.Ignore
            }
            else -> when (control) {
                "dpad_left" -> SettingsNavIntent.PrevItem
                "dpad_right" -> SettingsNavIntent.NextItem
                "dpad_down" -> SettingsNavIntent.Open
                "cross" -> SettingsNavIntent.Activate
                "circle" -> SettingsNavIntent.LeaveOrParent
                else -> SettingsNavIntent.Ignore
            }
        }
        "categories" -> when {
            isLandscape -> when (control) {
                "dpad_up" -> SettingsNavIntent.PrevItem
                "dpad_down" -> SettingsNavIntent.NextItem
                "dpad_right" -> SettingsNavIntent.Open
                "dpad_left", "circle" -> SettingsNavIntent.LeaveOrParent
                "cross" -> SettingsNavIntent.Activate
                else -> SettingsNavIntent.Ignore
            }
            else -> when (control) {
                "dpad_left" -> SettingsNavIntent.PrevItem
                "dpad_right" -> SettingsNavIntent.NextItem
                "dpad_up", "circle" -> SettingsNavIntent.LeaveOrParent
                "dpad_down" -> SettingsNavIntent.Open
                "cross" -> SettingsNavIntent.Activate
                else -> SettingsNavIntent.Ignore
            }
        }
        "content" -> when (control) {
            "circle" -> SettingsNavIntent.LeaveOrParent
            "dpad_left" -> if (isLandscape) SettingsNavIntent.LeaveOrParent else SettingsNavIntent.Ignore
            else -> SettingsNavIntent.Ignore
        }
        else -> SettingsNavIntent.Ignore
    }
}
