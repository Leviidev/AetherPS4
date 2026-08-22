package com.bachatas4.android.feature.settings

import org.junit.Assert.assertEquals
import org.junit.Test

class SettingsNavTest {
    @Test
    fun landscapeRailUsesVerticalAxis() {
        assertEquals(SettingsNavIntent.PrevItem, mapSettingsNav("tabs", "dpad_up", true))
        assertEquals(SettingsNavIntent.NextItem, mapSettingsNav("tabs", "dpad_down", true))
        assertEquals(SettingsNavIntent.Ignore, mapSettingsNav("tabs", "dpad_left", true))
        assertEquals(SettingsNavIntent.Open, mapSettingsNav("tabs", "dpad_right", true))
        assertEquals(SettingsNavIntent.Open, mapSettingsNav("tabs", "cross", true))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("tabs", "circle", true))
    }

    @Test
    fun portraitTabsKeepHorizontalAxis() {
        assertEquals(SettingsNavIntent.PrevItem, mapSettingsNav("tabs", "dpad_left", false))
        assertEquals(SettingsNavIntent.NextItem, mapSettingsNav("tabs", "dpad_right", false))
        assertEquals(SettingsNavIntent.Open, mapSettingsNav("tabs", "dpad_down", false))
        assertEquals(SettingsNavIntent.Activate, mapSettingsNav("tabs", "cross", false))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("tabs", "circle", false))
        assertEquals(SettingsNavIntent.Ignore, mapSettingsNav("tabs", "dpad_up", false))
    }

    @Test
    fun landscapeCategoriesUseVerticalAxis() {
        assertEquals(SettingsNavIntent.PrevItem, mapSettingsNav("categories", "dpad_up", true))
        assertEquals(SettingsNavIntent.NextItem, mapSettingsNav("categories", "dpad_down", true))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("categories", "dpad_left", true))
        assertEquals(SettingsNavIntent.Activate, mapSettingsNav("categories", "cross", true))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("categories", "circle", true))
        assertEquals(SettingsNavIntent.Open, mapSettingsNav("categories", "dpad_right", true))
    }

    @Test
    fun portraitCategoriesKeepHorizontalAxis() {
        assertEquals(SettingsNavIntent.PrevItem, mapSettingsNav("categories", "dpad_left", false))
        assertEquals(SettingsNavIntent.NextItem, mapSettingsNav("categories", "dpad_right", false))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("categories", "dpad_up", false))
        assertEquals(SettingsNavIntent.Open, mapSettingsNav("categories", "dpad_down", false))
        assertEquals(SettingsNavIntent.Activate, mapSettingsNav("categories", "cross", false))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("categories", "circle", false))
    }

    @Test
    fun contentBackIncludesLandscapeDpadLeft() {
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("content", "circle", true))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("content", "dpad_left", true))
        assertEquals(SettingsNavIntent.LeaveOrParent, mapSettingsNav("content", "circle", false))
        assertEquals(SettingsNavIntent.Ignore, mapSettingsNav("content", "dpad_left", false))
    }
}
