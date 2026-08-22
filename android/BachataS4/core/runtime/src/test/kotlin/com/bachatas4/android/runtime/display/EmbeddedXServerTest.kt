package com.bachatas4.android.runtime.display

import android.view.Surface
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class EmbeddedXServerTest {
    @Test
    fun surfaceDestroyedStopsServerOnce() = runTest {
        val server = FakeEmbeddedXServer()
        val lifecycle = EmbeddedXServerSurfaceLifecycle(server)

        lifecycle.surfaceDestroyed()
        lifecycle.surfaceDestroyed()

        assertEquals(1, server.stopCount)
    }

    @Test
    fun wrapperUsesDisplayZeroAndRejectsSecondStart() = runTest {
        val backend = FakeEmbeddedXServer()
        val server = SingleInstanceEmbeddedXServer(backend)

        assertEquals(":0", server.display)
        val surface = uninitializedSurface()
        server.start(surface = surface, width = 1920, height = 1080)
        val failure = runCatching { server.start(surface = surface, width = 1920, height = 1080) }.exceptionOrNull()

        assertTrue(failure is IllegalStateException)
    }

    @Test
    fun stopIsIdempotent() = runTest {
        val backend = FakeEmbeddedXServer()
        val server = SingleInstanceEmbeddedXServer(backend)

        server.stop()
        server.stop()

        assertEquals(1, backend.stopCount)
    }

    private class FakeEmbeddedXServer : EmbeddedXServer {
        override val display: String = ":99"
        var stopCount = 0

        override suspend fun start(surface: Surface, width: Int, height: Int) = Unit

        override suspend fun resize(width: Int, height: Int) = Unit

        override suspend fun stop() {
            stopCount++
        }
    }

    private fun uninitializedSurface(): Surface {
        val field = Class.forName("sun.misc.Unsafe").getDeclaredField("theUnsafe")
        field.isAccessible = true
        val unsafe = field.get(null) as sun.misc.Unsafe
        return unsafe.allocateInstance(Surface::class.java) as Surface
    }
}
