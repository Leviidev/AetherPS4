package com.bachatas4.android.runtime.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class RuntimeSettingCatalogTest {
    @Test
    fun duplicateNativeKeysAreRejected() {
        val error = assertThrows(IllegalArgumentException::class.java) {
            RuntimeSettingCatalog.parse(
                """[
                    {"id":"a","nativeKey":"x"},
                    {"id":"b","nativeKey":"x"}
                ]""".trimIndent(),
            )
        }

        assertTrue(error.message.orEmpty().contains("duplicate nativeKey x"))
    }

    @Test
    fun bundledCatalogContainsAllSupportedKinds() {
        val catalog = RuntimeSettingCatalog.loadFromResources()

        assertEquals(
            setOf(
                SettingKind.BOOLEAN,
                SettingKind.ENUM,
                SettingKind.INTEGER,
                SettingKind.DECIMAL,
                SettingKind.STRING,
                SettingKind.PATH,
                SettingKind.LIST,
            ),
            catalog.shadPs4.map { it.kind }.toSet(),
        )
        assertTrue(catalog.box64.size >= 100)
        assertTrue((catalog.shadPs4 + catalog.box64).all { it.title.isNotBlank() && it.help.isNotBlank() })
        val profile = catalog.box64.single { it.nativeKey == "BOX64_PROFILE" }
        assertEquals(SettingKind.ENUM, profile.kind)
        assertEquals(listOf("safest", "safe", "default", "fast", "fastest"), profile.choices)
    }

    @Test
    fun defaultMustMatchKindAndRange() {
        val wrongType = assertThrows(IllegalArgumentException::class.java) {
            RuntimeSettingCatalog.parse(
                """[{"id":"x","nativeKey":"x","kind":"INTEGER","defaultValue":"bad"}]""",
            )
        }
        assertTrue(wrongType.message.orEmpty().contains("default type"))

        val outsideRange = assertThrows(IllegalArgumentException::class.java) {
            RuntimeSettingCatalog.parse(
                """[{"id":"x","nativeKey":"x","kind":"INTEGER","defaultValue":5,"minimum":6}]""",
            )
        }
        assertTrue(outsideRange.message.orEmpty().contains("default range"))
    }
}
