package com.bachatas4.android.testing

import com.bachatas4.android.model.DiagnosticEvent
import com.bachatas4.android.model.LaunchRequest
import com.bachatas4.android.model.RuntimeState
import com.bachatas4.android.runtime.EmulatorRuntime
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.emptyFlow

class FakeEmulatorRuntime : EmulatorRuntime {
    private val mutableState = MutableStateFlow<RuntimeState>(RuntimeState.Idle)

    override val state: StateFlow<RuntimeState> = mutableState
    override val diagnostics: Flow<DiagnosticEvent> = emptyFlow()

    var lastLaunchRequest: LaunchRequest? = null
        private set

    override suspend fun verifyInstallation(): Result<Unit> = Result.success(Unit)

    override suspend fun launch(request: LaunchRequest): Result<String> {
        lastLaunchRequest = request
        val sessionId = "fake-session-${request.gameId}"
        mutableState.value = RuntimeState.Running(sessionId)
        return Result.success(sessionId)
    }

    override suspend fun stop(): Result<Unit> {
        mutableState.value = RuntimeState.Stopped(exitCode = 0)
        return Result.success(Unit)
    }
}
