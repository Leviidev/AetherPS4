package com.bachatas4.android

import android.os.Bundle
import android.util.Log
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import com.bachatas4.android.runtime.display.WinlatorEmbeddedXServer
import com.bachatas4.android.runtime.install.RuntimeInstaller
import com.bachatas4.android.runtime.install.RuntimeManifest
import com.bachatas4.android.runtime.process.RuntimeProbeExecutionMode
import com.bachatas4.android.runtime.process.RuntimeProbeLauncher
import com.bachatas4.android.runtime.process.RuntimeProbeRequest
import com.winlator.xconnector.UnixSocketConfig
import java.io.File
import java.nio.file.FileAlreadyExistsException
import java.nio.file.Path
import java.nio.file.Paths
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json

class Gate4Activity : ComponentActivity(), SurfaceHolder.Callback2 {
    private var probeJob: Job? = null
    private var server: WinlatorEmbeddedXServer? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setShowWhenLocked(true)
        setTurnScreenOn(true)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        setContentView(SurfaceView(this).also { it.holder.addCallback(this) })
        marker("ACTIVITY_CREATED")
    }

    override fun surfaceCreated(holder: SurfaceHolder) = Unit

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (width <= 0 || height <= 0 || probeJob?.isActive == true) return
        probeJob = lifecycleScope.launch { runAudioGate(holder, width, height) }
    }

    override fun surfaceRedrawNeeded(holder: SurfaceHolder) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        marker("SURFACE_DESTROYED")
        lifecycleScope.launch { stopGate() }
    }

    private suspend fun runAudioGate(holder: SurfaceHolder, width: Int, height: Int) {
        val socketRoot = File(cacheDir, "gate4-sockets").apply { mkdirs() }
        val activeServer = WinlatorEmbeddedXServer(
            context = this,
            socketRoot = socketRoot,
            useAbstractXSocket = true,
            useSharedMemoryAudio = false,
        )
        server = activeServer
        try {
            val runtimeRoot = withContext(Dispatchers.IO) { installRuntime() }
            activeServer.start(holder.surface, width, height)
            marker("BRIDGES_STARTED")
            val result = withContext(Dispatchers.IO) {
                RuntimeProbeLauncher().run(
                    RuntimeProbeRequest(
                        nativeLibraryDir = Paths.get(applicationInfo.nativeLibraryDir),
                        runtimeRoot = runtimeRoot,
                        executable = runtimeRoot.resolve("bin/audio-tone"),
                        environment = mapOf(
                            "HOME" to runtimeRoot.toString(),
                            "LD_LIBRARY_PATH" to listOf(
                                applicationInfo.nativeLibraryDir,
                                runtimeRoot.resolve("host"),
                            ).joinToString(":"),
                            "BOX64_PATH" to runtimeRoot.resolve("bin").toString(),
                            "BOX64_LD_LIBRARY_PATH" to listOf(
                                runtimeRoot.resolve("lib/x86_64-linux-gnu"),
                                runtimeRoot.resolve("lib64"),
                            ).joinToString(":"),
                            "BACHATA_ALSA_SOCKET" to File(
                                socketRoot,
                                UnixSocketConfig.ALSA_SERVER_PATH,
                            ).path,
                            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                        ),
                        executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC,
                    ),
                    timeoutSeconds = 30,
                )
            }
            result.output.lineSequence().filter(String::isNotBlank).forEach { marker("PROBE $it") }
            check(result.exitCode == 0) { "Audio probe exited ${result.exitCode}" }
            check("BACHATA_AUDIO_OK" in result.output) { "Audio success marker missing" }
            marker("AUDIO_OK")

            val vulkanResult = withContext(Dispatchers.IO) {
                RuntimeProbeLauncher().run(
                    RuntimeProbeRequest(
                        nativeLibraryDir = Paths.get(applicationInfo.nativeLibraryDir),
                        runtimeRoot = runtimeRoot,
                        executable = runtimeRoot.resolve("bin/vulkan-info"),
                        environment = mapOf(
                            "HOME" to runtimeRoot.toString(),
                            "LD_LIBRARY_PATH" to listOf(
                                applicationInfo.nativeLibraryDir,
                                runtimeRoot.resolve("host"),
                            ).joinToString(":"),
                            "BOX64_PATH" to runtimeRoot.resolve("bin").toString(),
                            "BOX64_LD_LIBRARY_PATH" to listOf(
                                runtimeRoot.resolve("lib/x86_64-linux-gnu"),
                                runtimeRoot.resolve("lib64"),
                            ).joinToString(":"),
                            "BOX64_EMULATED_LIBS" to listOf(
                                "libSDL2-2.0.so.0",
                                "libX11.so.6",
                                "libX11-xcb.so.1",
                                "libXcursor.so.1",
                                "libXext.so.6",
                                "libXfixes.so.3",
                                "libXi.so.6",
                                "libXrandr.so.2",
                                "libXrender.so.1",
                                "libXss.so.1",
                                "libxcb.so.1",
                                "libXau.so.6",
                                "libXdmcp.so.6",
                                "libxkbcommon.so.0",
                            ).joinToString(":"),
                            "DISPLAY" to activeServer.display,
                            "SDL_VIDEODRIVER" to "x11",
                            "TMPDIR" to cacheDir.path,
                            "XDG_CACHE_HOME" to File(cacheDir, "xdg").path,
                            "VK_ICD_FILENAMES" to runtimeRoot
                                .resolve("host/vulkan/icd.d/freedreno_icd.json")
                                .toString(),
                            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                        ),
                        executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC,
                    ),
                    timeoutSeconds = 30,
                )
            }
            vulkanResult.output.lineSequence().filter(String::isNotBlank).forEach {
                marker("VULKAN $it")
            }
            check(vulkanResult.exitCode == 0) { "Vulkan probe exited ${vulkanResult.exitCode}" }
            check("BACHATA_VULKAN_OK" in vulkanResult.output) { "Vulkan success marker missing" }
            marker("VULKAN_CAPABILITY_OK")
            marker("PROBES_OK")
        } catch (error: Exception) {
            Log.e(TAG, "BACHATA_GATE4_FAILED message=${error.message}", error)
        } finally {
            runCatching { activeServer.stop() }
            if (server === activeServer) server = null
            marker("BRIDGES_STOPPED")
        }
    }

    private suspend fun stopGate() {
        probeJob?.cancelAndJoin()
        probeJob = null
        server?.let { activeServer ->
            runCatching { activeServer.stop() }
            if (server === activeServer) server = null
        }
    }

    private fun installRuntime(): Path {
        val json = assets.open("runtime/manifest.json").bufferedReader().use { it.readText() }
        val manifest = Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(json)
        val installRoot = File(filesDir, "gate4-runtime").toPath()
        val target = installRoot.resolve(manifest.runtimeVersion)
        if (target.toFile().isDirectory) return target
        return assets.open("runtime/runtime.zip").use { bundle ->
            RuntimeInstaller(installRoot).install(bundle, manifest).getOrElse { error ->
                if (error is FileAlreadyExistsException && target.toFile().isDirectory) target else throw error
            }
        }
    }

    private fun marker(message: String) {
        Log.i(TAG, "BACHATA_GATE4_$message")
    }

    private companion object {
        const val TAG = "BachataGate4"
    }
}
