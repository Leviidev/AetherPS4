package com.bachatas4.android.runtime

import com.bachatas4.android.model.DiagnosticEvent
import com.bachatas4.android.model.LaunchRequest
import com.bachatas4.android.model.RuntimeState
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.StateFlow

interface EmulatorRuntime {
    val state: StateFlow<RuntimeState>
    val diagnostics: Flow<DiagnosticEvent>

    suspend fun verifyInstallation(): Result<Unit>

    suspend fun launch(request: LaunchRequest): Result<String>

    suspend fun stop(): Result<Unit>
}
