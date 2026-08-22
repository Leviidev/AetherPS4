package com.bachatas4.android

import android.util.Log
import androidx.test.platform.app.InstrumentationRegistry
import com.bachatas4.android.runtime.install.RuntimeInstaller
import com.bachatas4.android.runtime.install.RuntimeManifest
import com.bachatas4.android.runtime.process.RuntimeProbeExecutionMode
import com.bachatas4.android.runtime.process.RuntimeProbeLauncher
import com.bachatas4.android.runtime.process.RuntimeProbeRequest
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.util.Comparator
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Test

class FexCoreSmokeDeviceTest {
    @Test
    fun executesPinnedFexCoreOnDevice() {
        val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
        val cacheDir = targetContext.cacheDir.toPath().toRealPath()
        val installRoot = cacheDir.resolve("fexcore-smoke-${System.nanoTime()}")

        try {
            val manifest = targetContext.assets.open("runtime/manifest.json").bufferedReader().use {
                Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(it.readText())
            }
            val installed = targetContext.assets.open("runtime/runtime.zip").use { bundle ->
                RuntimeInstaller(installRoot).install(bundle, manifest).getOrThrow()
            }
            val marker =
                "FEXCORE_SMOKE_OK revision=f2b679f6028ce1c38875233aecfcf5d3f8ebecec gpr=ok stack=ok fp=ok threads=ok tls=ok callback=ok invalidation=ok"
            val result = RuntimeProbeLauncher().run(
                request = RuntimeProbeRequest(
                    nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
                    runtimeRoot = installed,
                    executable = installed.resolve("host/fexcore-smoke"),
                    environment = mapOf("GLIBC_TUNABLES" to "glibc.pthread.rseq=0"),
                    executionMode = RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE,
                    arguments = emptyList(),
                ),
                timeoutSeconds = 30,
            )

            val diagnostic = "FEXCore smoke exit=${result.exitCode}\n${result.output}"
            if (result.exitCode != 0) Log.e("BachataFexSmoke", diagnostic)
            assertEquals(diagnostic, 0, result.exitCode)
            assertEquals(diagnostic, marker, result.output.trim())
            Log.i("BachataFexSmoke", marker)
            println(marker)
        } finally {
            runCatching { cleanupUniqueChild(installRoot) }
                .onFailure { failure ->
                    Log.w("BachataFexSmoke", "FEXCore smoke cleanup failed: ${failure.message}", failure)
                }
        }
    }

    private fun cleanupUniqueChild(child: Path) {
        val cacheDir = InstrumentationRegistry.getInstrumentation().targetContext.cacheDir.toPath().toRealPath()
        check(child.parent == cacheDir) { "Refusing cleanup outside target cache child: $child" }
        check(child.fileName.toString().startsWith("fexcore-smoke-")) {
            "Refusing cleanup outside FEXCore smoke child: $child"
        }
        if (!Files.exists(child)) return

        Files.walk(child).use { paths ->
            paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
        }
    }
}
