package com.bachatas4.android.model

import org.junit.Assert.assertEquals
import org.junit.Test

class RuntimeStateTest {
    @Test
    fun failedStateKeepsStableCode() {
        val state = RuntimeState.Failed(
            code = RuntimeErrorCode.PROTOCOL_MISMATCH,
            detail = "Backend speaks protocol 2",
        )

        assertEquals(RuntimeErrorCode.PROTOCOL_MISMATCH, state.code)
        assertEquals("Backend speaks protocol 2", state.detail)
    }
}
