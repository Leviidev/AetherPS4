package com.bachatas4.android.runtime.input

import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class PhysicalBindingTest {
    private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true }

    @Test fun directionDefaultsToBoth() {
        val b = PhysicalBinding(PhysicalBindingKind.BUTTON, 96)
        assertEquals(AxisDirection.BOTH, b.direction)
    }

    @Test fun distinctDirectionsAreDistinctBindings() {
        val neg = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.NEGATIVE)
        val pos = PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.POSITIVE)
        assertNotEquals(neg, pos)
        assertNotEquals(neg.hashCode(), pos.hashCode())
    }

    @Test fun backwardCompatDeserializesTwoFieldJson() {
        // Old serialized profile had only kind + code, no direction field.
        val oldJson = """{"kind":"BUTTON","code":96}"""
        val b = json.decodeFromString<PhysicalBinding>(oldJson)
        assertEquals(PhysicalBinding(PhysicalBindingKind.BUTTON, 96), b)
        assertEquals(AxisDirection.BOTH, b.direction)
    }

    @Test fun roundTripSerializationPreservesDirection() {
        val original = PhysicalBinding(PhysicalBindingKind.AXIS, 15, AxisDirection.NEGATIVE)
        val encoded = json.encodeToString(PhysicalBinding.serializer(), original)
        val decoded = json.decodeFromString(PhysicalBinding.serializer(), encoded)
        assertEquals(original, decoded)
    }
}
