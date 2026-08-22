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

class Gate3Activity : ComponentActivity(), SurfaceHolder.Callback2 {
    private var probeJob: Job? = null
    private var xServer: WinlatorEmbeddedXServer? = null

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
        if (width <= 0 || height <= 0) return
        val active = xServer
        if (active != null) {
            lifecycleScope.launch {
                runCatching { active.resize(width, height) }
                    .onSuccess { marker("RESIZED ${width}x$height") }
                    .onFailure { failure("resize", it) }
            }
            return
        }
        if (probeJob?.isActive == true) return
        probeJob = lifecycleScope.launch {
            runGate(holder, width, height)
        }
    }

    override fun surfaceRedrawNeeded(holder: SurfaceHolder) = Unit

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        marker("SURFACE_DESTROYED")
        lifecycleScope.launch { stopGate() }
    }

    override fun onStop() {
        super.onStop()
        marker("ACTIVITY_STOPPED")
    }

    override fun onDestroy() {
        marker("ACTIVITY_DESTROYED")
        super.onDestroy()
    }

    private suspend fun runGate(holder: SurfaceHolder, width: Int, height: Int) {
        val server = WinlatorEmbeddedXServer(
            context = this,
            socketRoot = File(cacheDir, "gate3-sockets").apply { mkdirs() },
            useAbstractXSocket = true,
        )
        xServer = server
        try {
            val runtimeRoot = withContext(Dispatchers.IO) { installRuntime() }
            server.start(holder.surface, width, height)
            marker("XSERVER_STARTED display=${server.display} size=${width}x$height")
            val result = withContext(Dispatchers.IO) {
                RuntimeProbeLauncher().run(
                    RuntimeProbeRequest(
                        nativeLibraryDir = Paths.get(applicationInfo.nativeLibraryDir),
                        runtimeRoot = runtimeRoot,
                        executable = runtimeRoot.resolve("bin/sdl-window"),
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
                            "DISPLAY" to server.display,
                            "SDL_VIDEODRIVER" to "x11",
                            "TMPDIR" to cacheDir.path,
                            "XDG_CACHE_HOME" to File(cacheDir, "xdg").path,
                            "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                        ),
                        executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC,
                    ),
                )
            }
            result.output.lineSequence().filter(String::isNotBlank).forEach { marker("PROBE $it") }
            check(result.exitCode == 0) { "SDL probe exited ${result.exitCode}" }
            check("BACHATA_SDL_OK" in result.output) { "SDL success marker missing" }
            marker("OK")
        } catch (exception: Exception) {
            failure("gate", exception)
        } finally {
            runCatching { server.stop() }.onFailure { failure("stop", it) }
            if (xServer === server) xServer = null
            marker("XSERVER_STOPPED")
        }
    }

    private suspend fun stopGate() {
        probeJob?.cancelAndJoin()
        probeJob = null
        xServer?.let { server ->
            runCatching { server.stop() }.onFailure { failure("stop", it) }
            if (xServer === server) xServer = null
        }
    }

    private fun installRuntime(): Path {
        val json = assets.open("runtime/manifest.json").bufferedReader().use { it.readText() }
        val manifest = Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(json)
        val installRoot = File(filesDir, "gate3-runtime").toPath()
        val target = installRoot.resolve(manifest.runtimeVersion)
        if (target.toFile().isDirectory) return target
        return assets.open("runtime/runtime.zip").use { bundle ->
            RuntimeInstaller(installRoot).install(bundle, manifest).getOrElse { error ->
                if (error is FileAlreadyExistsException && target.toFile().isDirectory) target else throw error
            }
        }
    }

    private fun marker(message: String) {
        Log.i(TAG, "BACHATA_GATE3_$message")
    }

    private fun failure(stage: String, error: Throwable) {
        Log.e(TAG, "BACHATA_GATE3_FAILED stage=$stage message=${error.message}", error)
    }

    private companion object {
        const val TAG = "BachataGate3"
    }
}
