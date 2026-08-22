package com.bachatas4.android

import android.net.LocalServerSocket
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.os.Bundle
import android.util.Log
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.lifecycle.lifecycleScope
import com.bachatas4.android.runtime.install.RuntimeInstaller
import com.bachatas4.android.runtime.install.RuntimeManifest
import com.bachatas4.android.runtime.process.RuntimeProbeExecutionMode
import com.bachatas4.android.runtime.process.RuntimeProbeLauncher
import com.bachatas4.android.runtime.process.RuntimeProbeRequest
import java.io.File
import java.nio.file.FileAlreadyExistsException
import java.nio.file.Path
import java.nio.file.Paths
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json

class Gate5Activity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(TextView(this).apply { text = "Bachata Gate 5" })
        marker("ACTIVITY_CREATED")
        lifecycleScope.launch {
            runCatching { withContext(Dispatchers.IO) { runGate() } }
                .onFailure { error -> Log.e(TAG, "BACHATA_GATE5_FAILED message=${error.message}", error) }
        }
    }

    private fun runGate() {
        val runtimeRoot = installRuntime()
        runtimeRoot.resolve(".local/share").toFile().mkdirs()
        runtimeRoot.resolve(".config").toFile().mkdirs()
        val socketFile = File(filesDir, "gate5.sock").apply { delete() }
        val boundSocket = LocalSocket()
        var server: LocalServerSocket? = null
        var client: LocalSocket? = null
        var process: Process? = null
        val acceptExecutor = Executors.newSingleThreadExecutor()
        try {
            boundSocket.bind(
                LocalSocketAddress(socketFile.path, LocalSocketAddress.Namespace.FILESYSTEM),
            )
            val activeServer = LocalServerSocket(boundSocket.fileDescriptor)
            server = activeServer
            marker("SOCKET_READY")

            val environment = mapOf(
                "HOME" to runtimeRoot.toString(),
                "BOX64_PATH" to runtimeRoot.resolve("bin").toString(),
                "BOX64_LOG" to "1",
                "BOX64_LD_LIBRARY_PATH" to listOf(
                    runtimeRoot.resolve("lib/x86_64-linux-gnu"),
                    runtimeRoot.resolve("lib64"),
                ).joinToString(":"),
                "BOX64_EMULATED_LIBS" to "libudev.so.1:libuuid.so.1",
                "GLIBC_TUNABLES" to "glibc.pthread.rseq=0",
                "TMPDIR" to cacheDir.path,
                "XDG_CACHE_HOME" to File(cacheDir, "gate5-xdg").apply { mkdirs() }.path,
            )
            val processBuilder = RuntimeProbeLauncher().processBuilder(
                RuntimeProbeRequest(
                    nativeLibraryDir = Paths.get(applicationInfo.nativeLibraryDir),
                    runtimeRoot = runtimeRoot,
                    executable = runtimeRoot.resolve("bin/shadps4"),
                    environment = environment,
                    executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC,
                    arguments = listOf(
                        "--override-root",
                        filesDir.path,
                        "--bachata-socket",
                        socketFile.path,
                    ),
                ),
            )
            val activeProcess = processBuilder.start()
            process = activeProcess
            val outputReader = Thread {
                activeProcess.inputStream.bufferedReader().useLines { lines ->
                    lines.take(MAX_PROCESS_LINES).forEach { marker("PROCESS $it") }
                }
            }.apply { start() }

            val acceptFuture = acceptExecutor.submit<LocalSocket> { activeServer.accept() }
            while (client == null) {
                client = try {
                    acceptFuture.get(ACCEPT_POLL_MILLIS, TimeUnit.MILLISECONDS)
                } catch (_: TimeoutException) {
                    check(activeProcess.isAlive) {
                        outputReader.join(PROCESS_READER_JOIN_MILLIS)
                        "shadPS4 exited before socket connect: ${activeProcess.exitValue()}"
                    }
                    null
                }
            }
            val activeClient = checkNotNull(client)
            activeClient.soTimeout = TIMEOUT_SECONDS * 1_000
            val frames = activeClient.inputStream.bufferedReader().useLines { lines ->
                lines.take(EXPECTED_FRAMES.size).onEach { marker("FRAME $it") }.toList()
            }
            check(activeProcess.waitFor(TIMEOUT_SECONDS.toLong(), TimeUnit.SECONDS)) {
                "shadPS4 did not exit"
            }
            outputReader.join(PROCESS_READER_JOIN_MILLIS)
            marker("PROCESS_EXIT ${activeProcess.exitValue()}")
            check(frames == EXPECTED_FRAMES) { "Unexpected runtime frames: $frames" }
            check(activeProcess.exitValue() == 1) {
                "Expected validation exit 1, got ${activeProcess.exitValue()}"
            }
            marker("OK")
        } finally {
            if (process?.isAlive == true) process.destroyForcibly()
            runCatching { client?.close() }
            runCatching { server?.close() }
            runCatching { boundSocket.close() }
            acceptExecutor.shutdownNow()
            socketFile.delete()
        }
    }

    private fun installRuntime(): Path {
        val json = assets.open("runtime/manifest.json").bufferedReader().use { it.readText() }
        val manifest = Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(json)
        val installRoot = File(filesDir, "gate5-runtime").toPath()
        val target = installRoot.resolve(manifest.runtimeVersion)
        if (target.toFile().isDirectory) return target
        return assets.open("runtime/runtime.zip").use { bundle ->
            RuntimeInstaller(installRoot).install(bundle, manifest).getOrElse { error ->
                if (error is FileAlreadyExistsException && target.toFile().isDirectory) target else throw error
            }
        }
    }

    private fun marker(message: String) {
        Log.i(TAG, "BACHATA_GATE5_$message")
    }

    private companion object {
        const val TAG = "BachataGate5"
        const val TIMEOUT_SECONDS = 30
        const val MAX_PROCESS_LINES = 200
        const val ACCEPT_POLL_MILLIS = 250L
        const val PROCESS_READER_JOIN_MILLIS = 2_000L
        val EXPECTED_FRAMES = listOf(
            "BACHATA/1 HELLO version=1",
            "BACHATA/1 EVENT Starting",
            "BACHATA/1 ERROR code=CONTENT_INVALID",
            "BACHATA/1 EVENT Stopped exit_code=1",
        )
    }
}
