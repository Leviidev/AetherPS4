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

class FexGuestHarnessDeviceTest {
    @Test
    fun executesControlledFexGuestOnDevice() {
        val targetContext = InstrumentationRegistry.getInstrumentation().targetContext
        val cacheDir = targetContext.cacheDir.toPath().toRealPath()
        val installRoot = cacheDir.resolve("fexcore-guest-harness-${System.nanoTime()}")

        try {
            val manifest = targetContext.assets.open("runtime/manifest.json").bufferedReader().use {
                Json { ignoreUnknownKeys = true }.decodeFromString<RuntimeManifest>(it.readText())
            }
            val installed = targetContext.assets.open("runtime/runtime.zip").use { bundle ->
                RuntimeInstaller(installRoot).install(bundle, manifest).getOrThrow()
            }
            val phase1Marker =
                "FEXCORE_GUEST_ENGINE_OK revision=f2b679f6028ce1c38875233aecfcf5d3f8ebecec gpr=ok rflags=ok xmm=ok bridge=ok threads=ok tls=ok invalidation=ok teardown=ok"
            val marker =
                "$phase1Marker\nFEXCORE_GUEST_CPU_OK caller_mapping=ok thread_lifetime=ok invalidation=ok thread_isolation=ok overlap_rejected=ok nested_callback=ok\nHLE_VENEER_OK scalar=ok pointer=ok function_pointer=ok vector=ok stack=ok mapping=ok"
            val result = RuntimeProbeLauncher().run(
                request = RuntimeProbeRequest(
                    nativeLibraryDir = File(targetContext.applicationInfo.nativeLibraryDir).toPath(),
                    runtimeRoot = installed,
                    executable = installed.resolve("host/fexcore-guest-harness"),
                    environment = mapOf("GLIBC_TUNABLES" to "glibc.pthread.rseq=0"),
                    executionMode = RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE,
                ),
                timeoutSeconds = 30,
            )

            val diagnostic = "FEXCore guest harness exit=${result.exitCode}\n${result.output}"
            assertEquals(diagnostic, 0, result.exitCode)
            assertEquals(diagnostic, marker, result.output.trim())
            Log.i("BachataFexGuestHarness", marker)
            println(marker)
        } finally {
            cleanupUniqueChild(installRoot)
        }
    }

    private fun cleanupUniqueChild(child: Path) {
        val cacheDir = InstrumentationRegistry.getInstrumentation().targetContext.cacheDir.toPath().toRealPath()
        check(child.parent == cacheDir) { "Refusing cleanup outside target cache child: $child" }
        check(child.fileName.toString().startsWith("fexcore-guest-harness-")) {
            "Refusing cleanup outside FEXCore guest harness child: $child"
        }
        if (!Files.exists(child)) return

        Files.walk(child).use { paths ->
            paths.sorted(Comparator.reverseOrder()).forEach(Files::deleteIfExists)
        }
    }
}
