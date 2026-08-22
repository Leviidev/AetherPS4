package com.bachatas4.android.feature.session

import android.content.Context
import android.content.Intent
import android.app.ActivityManager
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.bachatas4.android.data.GameRepository
import com.bachatas4.android.model.RuntimeErrorCode
import com.bachatas4.android.runtime.session.ManagedSession
import com.bachatas4.android.runtime.session.ManagedSessionState
import com.bachatas4.android.runtime.process.RuntimeVulkanDriver
import com.bachatas4.android.runtime.process.RuntimeVulkanDriverPreference
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import javax.inject.Inject
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow

data class DeviceTelemetry(val ramUsedMb: Long = 0, val ramTotalMb: Long = 0, val gpuLoad: String = "N/A")

@HiltViewModel
class SessionViewModel @Inject constructor(
    private val repository: GameRepository,
    @ApplicationContext private val context: Context,
) : ViewModel() {
    val state: StateFlow<ManagedSessionState> = ManagedSession.state
    val frameTelemetry = ManagedSession.frameTelemetry
    private val mutableDeviceTelemetry = MutableStateFlow(DeviceTelemetry())
    val deviceTelemetry: StateFlow<DeviceTelemetry> = mutableDeviceTelemetry

    init {
        viewModelScope.launch {
            val activityManager = context.getSystemService(ActivityManager::class.java)
            while (true) {
                ManagedSession.refreshFrameTelemetry()
                val memory = ActivityManager.MemoryInfo().also { activityManager.getMemoryInfo(it) }
                val gpu = runCatching {
                    java.io.File("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage").readText().trim()
                }.getOrNull()?.takeIf(String::isNotBlank) ?: "N/A"
                mutableDeviceTelemetry.value = DeviceTelemetry(
                    ramUsedMb = (memory.totalMem - memory.availMem) / (1024 * 1024),
                    ramTotalMb = memory.totalMem / (1024 * 1024),
                    gpuLoad = gpu,
                )
                delay(500)
            }
        }
    }

    fun launch(gameId: String) {
        if (state.value is ManagedSessionState.Running || state.value is ManagedSessionState.Preparing) return
        viewModelScope.launch {
            val game = repository.getGame(gameId)
            if (game == null) {
                ManagedSession.update(ManagedSessionState.Failed(RuntimeErrorCode.CONTENT_INVALID, "Game not found"))
                return@launch
            }
            context.startService(
                // Read once: changing Settings cannot mutate an active session.
                Intent(ManagedSession.ACTION_START).setClassName(context.packageName, ManagedSession.SERVICE_CLASS)
                    .putExtra(ManagedSession.EXTRA_GAME_ID, game.id)
                    .putExtra(ManagedSession.EXTRA_GAME_PATH, game.relativePath)
                    .putExtra(
                        ManagedSession.EXTRA_VULKAN_DRIVER,
                        RuntimeVulkanDriverPreference.decode(
                            context.getSharedPreferences(
                                RuntimeVulkanDriverPreference.FILE_NAME,
                                Context.MODE_PRIVATE,
                            ).getString(RuntimeVulkanDriverPreference.KEY, null),
                        ).name,
                    ),
            )
        }
    }
}
