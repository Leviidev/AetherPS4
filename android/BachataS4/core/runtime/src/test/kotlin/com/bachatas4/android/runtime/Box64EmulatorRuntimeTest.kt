package com.bachatas4.android.runtime

import com.bachatas4.android.model.LaunchRequest
import com.bachatas4.android.model.RuntimeState
import com.bachatas4.android.runtime.process.RuntimeProcessHandle
import com.bachatas4.android.runtime.process.RuntimeProcessRequest
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import com.bachatas4.android.runtime.protocol.RuntimeMessage
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class Box64EmulatorRuntimeTest {
    @Test
    fun launchTransitionsPreparingToRunningAndRejectsSecondSession() = runTest {
        val enteredLauncher = CompletableDeferred<Unit>()
        val releaseLauncher = CompletableDeferred<Unit>()
        val process = FakeOwnedProcess()
        var launchCount = 0
        val runtime = runtime(
            processLauncher = {
                launchCount++
                enteredLauncher.complete(Unit)
                releaseLauncher.await()
                process
            },
        )

        assertEquals(RuntimeState.Idle, runtime.state.value)
        val first = async { runtime.launch(LAUNCH_REQUEST) }
        enteredLauncher.await()
        assertEquals(RuntimeState.Preparing("launch"), runtime.state.value)
        releaseLauncher.complete(Unit)
        val sessionId = first.await().getOrThrow()

        assertEquals("session-1", sessionId)
        assertEquals(RuntimeState.Running("session-1"), runtime.state.value)
        assertTrue(runtime.launch(LAUNCH_REQUEST).isFailure)
        assertEquals(1, launchCount)
    }

    @Test
    fun stopSendsProtocolAndEscalatesOwnedProcessAtExactTimeouts() = runTest {
        val process = FakeOwnedProcess()
        val messages = mutableListOf<RuntimeMessage>()
        val waits = mutableListOf<Long>()
        val runtime = runtime(
            processLauncher = { process },
            controlChannel = { messages += it },
            processWaiter = { owned, timeout ->
                assertTrue(owned === process)
                waits += timeout
                false
            },
        )
        runtime.launch(LAUNCH_REQUEST).getOrThrow()

        runtime.stop().getOrThrow()

        assertEquals(listOf(RuntimeMessage.Stop), messages)
        assertEquals(listOf(5_000L, 2_000L), waits)
        assertTrue(process.destroyed)
        assertTrue(process.destroyedForcibly)
        assertEquals(RuntimeState.Stopped(-1), runtime.state.value)
    }

    @Test
    fun gracefulStopDoesNotDestroyProcess() = runTest {
        val process = FakeOwnedProcess(exitCode = 0)
        val runtime = runtime(
            processLauncher = { process },
            processWaiter = { _, _ -> true },
        )
        runtime.launch(LAUNCH_REQUEST).getOrThrow()

        runtime.stop().getOrThrow()

        assertFalse(process.destroyed)
        assertFalse(process.destroyedForcibly)
        assertEquals(RuntimeState.Stopped(0), runtime.state.value)
    }

    @Test
    fun cancelledStopStillFinishesOwnedProcessCleanup() = runTest {
        val waitStarted = CompletableDeferred<Unit>()
        val releaseWait = CompletableDeferred<Unit>()
        var launchCount = 0
        val runtime = runtime(
            processLauncher = {
                launchCount++
                FakeOwnedProcess(exitCode = 0)
            },
            processWaiter = { _, _ ->
                waitStarted.complete(Unit)
                releaseWait.await()
                true
            },
        )
        runtime.launch(LAUNCH_REQUEST).getOrThrow()
        val stop = launch { runtime.stop() }
        waitStarted.await()

        stop.cancel()
        releaseWait.complete(Unit)
        stop.join()

        assertEquals(RuntimeState.Stopped(0), runtime.state.value)
        assertTrue(runtime.launch(LAUNCH_REQUEST).isSuccess)
        assertEquals(2, launchCount)
    }

    @Test
    fun hangingStopSenderTimesOutBeforeProcessEscalation() = runTest {
        val process = FakeOwnedProcess()
        val waits = mutableListOf<Long>()
        val runtime = runtime(
            processLauncher = { process },
            controlChannel = { awaitCancellation() },
            processWaiter = { _, timeout ->
                waits += timeout
                false
            },
            stopMessageTimeoutMillis = 50L,
        )
        runtime.launch(LAUNCH_REQUEST).getOrThrow()

        val result = runtime.stop()

        assertTrue(result.isFailure)
        assertTrue(result.exceptionOrNull()!!.message!!.contains("Timed out sending Stop"))
        assertEquals(listOf(5_000L, 2_000L), waits)
        assertTrue(process.destroyed)
        assertTrue(process.destroyedForcibly)
        assertEquals(RuntimeState.Stopped(-1), runtime.state.value)
    }

    @Test
    fun forceDestroyFailureDoesNotMaskWaitFailureOrKeepOwnership() = runTest {
        val waitFailure = IllegalStateException("wait failed")
        val forceFailure = IllegalStateException("force failed")
        var launchCount = 0
        val runtime = runtime(
            processLauncher = {
                launchCount++
                FakeOwnedProcess(forceFailure = forceFailure)
            },
            processWaiter = { _, _ -> throw waitFailure },
        )
        runtime.launch(LAUNCH_REQUEST).getOrThrow()

        val result = runtime.stop()

        assertTrue(result.isFailure)
        assertTrue(result.exceptionOrNull() === waitFailure)
        assertEquals(listOf(forceFailure), waitFailure.suppressed.toList())
        assertEquals(RuntimeState.Stopped(-1), runtime.state.value)
        assertTrue(runtime.launch(LAUNCH_REQUEST).isSuccess)
        assertEquals(2, launchCount)
    }

    @Test
    fun forceDestroyFailureDoesNotMaskCancellation() = runTest {
        val cancellation = CancellationException("wait cancelled")
        val forceFailure = IllegalStateException("force failed")
        val runtime = runtime(
            processLauncher = { FakeOwnedProcess(forceFailure = forceFailure) },
            processWaiter = { _, _ -> throw cancellation },
        )
        runtime.launch(LAUNCH_REQUEST).getOrThrow()

        val thrown = runCatching { runtime.stop() }.exceptionOrNull()

        assertTrue(thrown === cancellation)
        assertEquals(listOf(forceFailure), cancellation.suppressed.toList())
        assertEquals(RuntimeState.Stopped(-1), runtime.state.value)
    }

    private fun runtime(
        processLauncher: RuntimeProcessFactory,
        controlChannel: RuntimeControlChannel = RuntimeControlChannel { },
        processWaiter: RuntimeProcessWaiter = RuntimeProcessWaiter { _, _ -> true },
        stopMessageTimeoutMillis: Long = 1_000L,
    ) = Box64EmulatorRuntime(
        installationVerifier = RuntimeInstallationVerifier { },
        processLauncher = processLauncher,
        requestFactory = RuntimeProcessRequestFactory { _, _ -> PROCESS_REQUEST },
        controlChannel = controlChannel,
        processWaiter = processWaiter,
        stopMessageTimeoutMillis = stopMessageTimeoutMillis,
        sessionIdProvider = SessionIdProvider { "session-1" },
    )

    private companion object {
        val LAUNCH_REQUEST = LaunchRequest("CUSA00001", "game/eboot.bin")
        val PROCESS_REQUEST = RuntimeProcessRequest(
            nativeLibraryDir = Path.of("/apk/lib"),
            runtimeRoot = Path.of("/data/runtime"),
            overrideRoot = Path.of("/data"),
            shadPs4Executable = Path.of("/data/runtime/bin/shadps4"),
            socketPath = "/data/runtime/session.sock",
            guestBackend = RuntimeGuestBackend.BOX64,
        )
    }
}

private class FakeOwnedProcess(
    override val exitCode: Int? = null,
    private val forceFailure: Exception? = null,
) : RuntimeProcessHandle {
    override val isAlive: Boolean
        get() = !destroyed && !destroyedForcibly && exitCode == null

    var destroyed = false
    var destroyedForcibly = false

    override fun destroy() {
        destroyed = true
    }

    override fun destroyForcibly() {
        destroyedForcibly = true
        forceFailure?.let { throw it }
    }

    override fun waitFor(timeout: Long, unit: TimeUnit): Boolean = false
}
