package com.bachatas4.android.testing

import com.bachatas4.android.model.LaunchRequest
import com.bachatas4.android.model.RuntimeState
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FakeEmulatorRuntimeTest {
    @Test
    fun launchRecordsRequestAndTransitionsToRunning() = runTest {
        val runtime = FakeEmulatorRuntime()
        val request = LaunchRequest(gameId = "CUSA00001", ebootPath = "game/eboot.bin")

        assertEquals(RuntimeState.Idle, runtime.state.value)

        val sessionId = runtime.launch(request).getOrThrow()

        assertEquals(request, runtime.lastLaunchRequest)
        assertTrue(sessionId.isNotBlank())
        assertEquals(RuntimeState.Running(sessionId), runtime.state.value)
    }

    @Test
    fun stopTransitionsRunningRuntimeToStopped() = runTest {
        val runtime = FakeEmulatorRuntime()
        runtime.launch(LaunchRequest(gameId = "CUSA00001", ebootPath = "game/eboot.bin"))

        runtime.stop().getOrThrow()

        assertEquals(RuntimeState.Stopped(exitCode = 0), runtime.state.value)
    }
}
