package com.bachatas4.android.runtime.display

import android.view.Surface
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock

interface EmbeddedXServer {
    val display: String

    suspend fun start(surface: Surface, width: Int, height: Int)

    suspend fun resize(width: Int, height: Int)

    suspend fun stop()
}

class SingleInstanceEmbeddedXServer(
    private val backend: EmbeddedXServer,
) : EmbeddedXServer {
    override val display: String = ":0"

    private val mutex = Mutex()
    private var started = false
    private var stopped = false

    override suspend fun start(surface: Surface, width: Int, height: Int) = mutex.withLock {
        require(width > 0 && height > 0) { "X server dimensions must be positive" }
        check(!started && !stopped) { "Embedded X server already started or stopped" }
        backend.start(surface, width, height)
        started = true
    }

    override suspend fun resize(width: Int, height: Int) = mutex.withLock {
        require(width > 0 && height > 0) { "X server dimensions must be positive" }
        check(started && !stopped) { "Embedded X server is not running" }
        backend.resize(width, height)
    }

    override suspend fun stop() = mutex.withLock {
        if (stopped) return@withLock
        backend.stop()
        stopped = true
    }
}

class EmbeddedXServerSurfaceLifecycle(
    private val server: EmbeddedXServer,
) {
    private val mutex = Mutex()
    private var destroyed = false

    suspend fun surfaceDestroyed() = mutex.withLock {
        if (destroyed) return@withLock
        server.stop()
        destroyed = true
    }
}
