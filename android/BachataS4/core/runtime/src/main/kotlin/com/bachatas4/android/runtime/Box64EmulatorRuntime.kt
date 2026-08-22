package com.bachatas4.android.runtime

import com.bachatas4.android.model.DiagnosticEvent
import com.bachatas4.android.model.LaunchRequest
import com.bachatas4.android.model.RuntimeErrorCode
import com.bachatas4.android.model.RuntimeState
import com.bachatas4.android.runtime.process.RuntimeProcessHandle
import com.bachatas4.android.runtime.process.RuntimeProcessRequest
import com.bachatas4.android.runtime.protocol.RuntimeMessage
import java.util.UUID
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout

fun interface RuntimeInstallationVerifier {
    suspend fun verify()
}

fun interface RuntimeProcessFactory {
    suspend fun launch(request: RuntimeProcessRequest): RuntimeProcessHandle
}

fun interface RuntimeProcessRequestFactory {
    fun create(request: LaunchRequest, sessionId: String): RuntimeProcessRequest
}

fun interface RuntimeControlChannel {
    suspend fun send(message: RuntimeMessage)
}

fun interface RuntimeProcessWaiter {
    suspend fun waitFor(process: RuntimeProcessHandle, timeoutMillis: Long): Boolean
}

fun interface SessionIdProvider {
    fun next(): String
}

fun interface TimestampProvider {
    fun nowMillis(): Long
}

class Box64EmulatorRuntime(
    private val installationVerifier: RuntimeInstallationVerifier,
    private val processLauncher: RuntimeProcessFactory,
    private val requestFactory: RuntimeProcessRequestFactory,
    private val controlChannel: RuntimeControlChannel,
    private val processWaiter: RuntimeProcessWaiter,
    private val stopMessageTimeoutMillis: Long = STOP_MESSAGE_TIMEOUT_MILLIS,
    private val sessionIdProvider: SessionIdProvider = SessionIdProvider {
        UUID.randomUUID().toString()
    },
    private val timestampProvider: TimestampProvider = TimestampProvider(System::currentTimeMillis),
) : EmulatorRuntime {
    init {
        require(stopMessageTimeoutMillis > 0) { "Stop message timeout must be positive" }
    }

    private val sessionMutex = Mutex()
    private val mutableState = MutableStateFlow<RuntimeState>(RuntimeState.Idle)
    private val mutableDiagnostics = MutableSharedFlow<DiagnosticEvent>(extraBufferCapacity = 32)
    private var ownedProcess: RuntimeProcessHandle? = null

    override val state: StateFlow<RuntimeState> = mutableState
    override val diagnostics: Flow<DiagnosticEvent> = mutableDiagnostics.asSharedFlow()

    override suspend fun verifyInstallation(): Result<Unit> = resultOf {
        installationVerifier.verify()
    }

    override suspend fun launch(request: LaunchRequest): Result<String> = sessionMutex.withLock {
        if (ownedProcess != null || mutableState.value is RuntimeState.Preparing ||
            mutableState.value is RuntimeState.Running || mutableState.value is RuntimeState.Stopping
        ) {
            return@withLock Result.failure(IllegalStateException("A runtime session is active"))
        }

        mutableState.value = RuntimeState.Preparing(stage = "launch")
        val sessionId = sessionIdProvider.next()
        var startedProcess: RuntimeProcessHandle? = null
        try {
            val processRequest = requestFactory.create(request, sessionId)
            startedProcess = processLauncher.launch(processRequest)
            currentCoroutineContext().ensureActive()
            ownedProcess = startedProcess
            mutableState.value = RuntimeState.Running(sessionId)
            Result.success(sessionId)
        } catch (cancellation: CancellationException) {
            try {
                startedProcess?.destroyForcibly()
            } catch (cleanupFailure: Exception) {
                cancellation.addSuppressed(cleanupFailure)
            } finally {
                mutableState.value = RuntimeState.Idle
            }
            throw cancellation
        } catch (exception: Exception) {
            try {
                startedProcess?.destroyForcibly()
            } catch (cleanupFailure: Exception) {
                exception.addSuppressed(cleanupFailure)
            } finally {
                mutableState.value = RuntimeState.Failed(
                    RuntimeErrorCode.TRANSLATOR_START_FAILED,
                    exception.message ?: exception.javaClass.simpleName,
                )
            }
            emitDiagnostic("launch", exception)
            Result.failure(exception)
        }
    }

    override suspend fun stop(): Result<Unit> = sessionMutex.withLock {
        val process = ownedProcess
            ?: return@withLock Result.failure(IllegalStateException("No runtime session is active"))
        mutableState.value = RuntimeState.Stopping
        var failure: Exception? = null
        var explicitCancellation: CancellationException? = null

        fun recordFailure(exception: Exception) {
            val primary = explicitCancellation ?: failure
            if (primary == null) {
                failure = exception
            } else if (primary !== exception) {
                primary.addSuppressed(exception)
            }
        }

        fun recordCancellation(cancellation: CancellationException) {
            val primary = explicitCancellation
            if (primary == null) {
                failure?.let(cancellation::addSuppressed)
                failure = null
                explicitCancellation = cancellation
            } else if (primary !== cancellation) {
                primary.addSuppressed(cancellation)
            }
        }

        fun forceDestroyPreservingFailure() {
            try {
                process.destroyForcibly()
            } catch (cleanupFailure: Exception) {
                recordFailure(cleanupFailure)
            }
        }

        withContext(NonCancellable) {
            try {
                try {
                    withTimeout(stopMessageTimeoutMillis) {
                        controlChannel.send(RuntimeMessage.Stop)
                    }
                } catch (timeout: TimeoutCancellationException) {
                    val exception = IllegalStateException(
                        "Timed out sending Stop after ${stopMessageTimeoutMillis}ms",
                        timeout,
                    )
                    recordFailure(exception)
                    emitDiagnostic("protocol", exception)
                } catch (cancellation: CancellationException) {
                    recordCancellation(cancellation)
                } catch (exception: Exception) {
                    recordFailure(exception)
                    emitDiagnostic("protocol", exception)
                }

                try {
                    if (!processWaiter.waitFor(process, GRACEFUL_STOP_MILLIS)) {
                        process.destroy()
                        if (!processWaiter.waitFor(process, DESTROY_STOP_MILLIS)) {
                            forceDestroyPreservingFailure()
                        }
                    }
                } catch (cancellation: CancellationException) {
                    recordCancellation(cancellation)
                    forceDestroyPreservingFailure()
                } catch (exception: Exception) {
                    recordFailure(exception)
                    forceDestroyPreservingFailure()
                    emitDiagnostic("process", exception)
                }
            } finally {
                ownedProcess = null
                val exitCode = try {
                    process.exitCode
                } catch (exitFailure: Exception) {
                    recordFailure(exitFailure)
                    null
                }
                mutableState.value = RuntimeState.Stopped(exitCode ?: UNKNOWN_EXIT_CODE)
            }
        }

        explicitCancellation?.let { throw it }
        failure?.let(Result.Companion::failure) ?: Result.success(Unit)
    }

    private fun emitDiagnostic(category: String, exception: Exception) {
        mutableDiagnostics.tryEmit(
            DiagnosticEvent(
                timestampMs = timestampProvider.nowMillis(),
                category = category,
                message = exception.message ?: exception.javaClass.simpleName,
            ),
        )
    }

    private suspend fun resultOf(block: suspend () -> Unit): Result<Unit> = try {
        block()
        Result.success(Unit)
    } catch (cancellation: CancellationException) {
        throw cancellation
    } catch (exception: Exception) {
        Result.failure(exception)
    }

    companion object {
        const val GRACEFUL_STOP_MILLIS = 5_000L
        const val DESTROY_STOP_MILLIS = 2_000L
        const val STOP_MESSAGE_TIMEOUT_MILLIS = 1_000L
        const val UNKNOWN_EXIT_CODE = -1

        fun defaultProcessWaiter(): RuntimeProcessWaiter = RuntimeProcessWaiter { process, timeout ->
            withContext(Dispatchers.IO) {
                process.waitFor(timeout, TimeUnit.MILLISECONDS)
            }
        }
    }
}
