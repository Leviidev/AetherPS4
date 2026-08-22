package com.bachatas4.android.runtime.process

import java.io.InterruptedIOException
import java.io.InputStream
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class RuntimeProbeLauncherTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun retainsOutputWhenKilledProcessClosesReader() {
        val output = StringBuilder()
        val stream = object : InputStream() {
            private val bytes = "before-timeout\n".encodeToByteArray()
            private var index = 0

            override fun read(): Int {
                if (index < bytes.size) return bytes[index++].toInt()
                throw InterruptedIOException("closed")
            }
        }

        readProcessOutput(stream, output)

        assertEquals("before-timeout\n", output.toString())
    }

    @Test
    fun buildsExactShellFreeCommand() {
        val request = validRequest()

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbox64.so").toRealPath().toString(),
                request.executable.toRealPath().toString(),
            ),
            RuntimeProbeLauncher().command(request),
        )
    }

    @Test
    fun preservesArgumentsWithoutShellInterpretation() {
        val request = validRequest().copy(
            arguments = listOf("--override-root", "/data/runtime", "/games/title;not-a-command"),
        )

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbox64.so").toRealPath().toString(),
                request.executable.toRealPath().toString(),
                "--override-root",
                "/data/runtime",
                "/games/title;not-a-command",
            ),
            RuntimeProbeLauncher().command(request),
        )
    }

    @Test
    fun rejectsNulInArgument() {
        val request = validRequest().copy(arguments = listOf("bad\u0000argument"))

        assertThrows(IllegalArgumentException::class.java) {
            RuntimeProbeLauncher().command(request)
        }
    }

    @Test
    fun buildsHostGlibcCommandThroughRuntimeLoader() {
        val request = validRequest(executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC)

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbachata_host_loader.so").toRealPath().toString(),
                "--library-path",
                request.runtimeRoot.resolve("host").toRealPath().toString(),
                request.nativeLibraryDir.resolve("libbachata_host_box64.so").toRealPath().toString(),
                request.executable.toRealPath().toString(),
            ),
            RuntimeProbeLauncher().command(request),
        )
    }

    @Test
    fun buildsHostGlibcNativeCommandWithoutBox64() {
        val request = nativeProbeRequest()

        val command = RuntimeProbeLauncher().command(request)

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbachata_host_loader.so").toRealPath().toString(),
                "--library-path",
                request.runtimeRoot.resolve("host").toRealPath().toString(),
                request.runtimeRoot.resolve("host/fexcore-smoke").toRealPath().toString(),
            ),
            command,
        )
        assertFalse(command.any { it.contains("box64", ignoreCase = true) })
    }

    @Test
    fun usesRequestedHostGlibcNativeProbe() {
        val request = nativeProbeRequest()
        val guestHarness = request.runtimeRoot.resolve("host/fexcore-guest-harness")
        Files.write(guestHarness, byteArrayOf(7))
        val guestRequest = request.copy(executable = guestHarness)

        assertEquals(
            listOf(
                request.nativeLibraryDir.resolve("libbachata_host_loader.so").toRealPath().toString(),
                "--library-path",
                request.runtimeRoot.resolve("host").toRealPath().toString(),
                guestHarness.toRealPath().toString(),
            ),
            RuntimeProbeLauncher().command(guestRequest),
        )
    }

    @Test
    fun clearsInheritedEnvironmentAndCopiesOnlyAllowlist() {
        val request = nativeProbeRequest(
            environment = mapOf(
                "HOME" to "/data/home",
                "DISPLAY" to ":0",
                "SDL_VIDEODRIVER" to "x11",
                "BOX64_LD_LIBRARY_PATH" to "/data/runtime/lib",
                "BOX64_EMULATED_LIBS" to "libSDL2-2.0.so.0",
                "BACHATA_ALSA_SOCKET" to "/data/cache/tmp/.sound/AS0",
                "PATH" to "/system/bin",
                "LD_PRELOAD" to "/evil.so",
            ),
        )

        val builder = RuntimeProbeLauncher().processBuilder(request)

        assertEquals(
            mapOf(
                "HOME" to "/data/home",
                "DISPLAY" to ":0",
                "SDL_VIDEODRIVER" to "x11",
                "BOX64_LD_LIBRARY_PATH" to "/data/runtime/lib",
                "BOX64_EMULATED_LIBS" to "libSDL2-2.0.so.0",
                "BACHATA_ALSA_SOCKET" to "/data/cache/tmp/.sound/AS0",
            ),
            builder.environment(),
        )
        assertFalse(builder.environment().containsKey("PATH"))
        assertFalse(builder.environment().containsKey("LD_PRELOAD"))
        assertTrue(builder.redirectErrorStream())
    }

    @Test
    fun rejectsProbeOutsideRuntimeRoot() {
        val request = validRequest()
        val outside = temporaryFolder.newFile("outside-probe").toPath()

        assertThrows(SecurityException::class.java) {
            RuntimeProbeLauncher().command(request.copy(executable = outside))
        }
    }

    @Test
    fun rejectsHostGlibcNativeProbeOutsideRuntimeRoot() {
        val request = nativeProbeRequest()
        val outside = temporaryFolder.newFile("outside-probe").toPath()

        assertThrows(SecurityException::class.java) {
            RuntimeProbeLauncher().command(request.copy(executable = outside))
        }
    }

    @Test
    fun rejectsHostGlibcNativeProbeThatIsNotARegularFile() {
        val request = nativeProbeRequest()
        Files.delete(request.executable)
        Files.createDirectories(request.executable)

        assertThrows(IllegalArgumentException::class.java) {
            RuntimeProbeLauncher().command(request)
        }
    }

    @Test
    fun rejectsHostGlibcNativeHostDirectoryOutsideRuntimeRoot() {
        val request = nativeProbeRequest()
        val hostDirectory = request.runtimeRoot.resolve("host")
        Files.delete(request.executable)
        Files.delete(hostDirectory)
        val outsideDirectory = temporaryFolder.newFolder("outside-host").toPath()
        Files.write(outsideDirectory.resolve("fexcore-smoke"), byteArrayOf(6))
        Files.createSymbolicLink(hostDirectory, outsideDirectory)

        assertThrows(SecurityException::class.java) {
            RuntimeProbeLauncher().command(request)
        }
    }

    @Test
    fun rejectsHostGlibcNativeLoaderOutsideNativeLibraryDirectory() {
        val request = nativeProbeRequest()
        val loader = request.nativeLibraryDir.resolve("libbachata_host_loader.so")
        Files.delete(loader)
        val outsideLoader = temporaryFolder.newFile("outside-loader").toPath()
        Files.createSymbolicLink(loader, outsideLoader)

        assertThrows(SecurityException::class.java) {
            RuntimeProbeLauncher().command(request)
        }
    }

    @Test
    fun promotesContainedProbeToOwnerExecutableBeforeLaunch() {
        val request = validRequest()
        assertFalse(Files.isExecutable(request.executable))

        RuntimeProbeLauncher().processBuilder(request)

        assertTrue(Files.isExecutable(request.executable))
    }

    @Test
    fun promotesHostGlibcExecutablesBeforeLaunch() {
        val request = validRequest(executionMode = RuntimeProbeExecutionMode.BOX64_HOST_GLIBC)
        val loader = request.nativeLibraryDir.resolve("libbachata_host_loader.so")
        val box64 = request.nativeLibraryDir.resolve("libbachata_host_box64.so")
        assertFalse(Files.isExecutable(loader))
        assertFalse(Files.isExecutable(box64))
        assertFalse(Files.isExecutable(request.executable))

        RuntimeProbeLauncher().processBuilder(request)

        assertTrue(Files.isExecutable(loader))
        assertTrue(Files.isExecutable(box64))
        assertTrue(Files.isExecutable(request.executable))
    }

    @Test
    fun doesNotPromoteHostGlibcNativeRunnerToExecutableBeforeLaunch() {
        val request = nativeProbeRequest()
        val loader = request.nativeLibraryDir.resolve("libbachata_host_loader.so")
        assertFalse(Files.isExecutable(loader))
        assertFalse(Files.isExecutable(request.executable))

        RuntimeProbeLauncher().processBuilder(request)

        assertTrue(Files.isExecutable(loader))
        assertFalse(Files.isExecutable(request.executable))
    }

    private fun validRequest(
        environment: Map<String, String> = emptyMap(),
        executionMode: RuntimeProbeExecutionMode = RuntimeProbeExecutionMode.BOX64_APK_NATIVE,
    ): RuntimeProbeRequest {
        val base = temporaryFolder.newFolder().toPath()
        val nativeLibraryDir = Files.createDirectories(base.resolve("apk/lib"))
        val box64 = Files.write(nativeLibraryDir.resolve("libbox64.so"), byteArrayOf(1))
        assertTrue(box64.toFile().setExecutable(true))
        Files.write(nativeLibraryDir.resolve("libbachata_host_loader.so"), byteArrayOf(3))
        Files.write(nativeLibraryDir.resolve("libbachata_host_box64.so"), byteArrayOf(4))
        val runtimeRoot = Files.createDirectories(base.resolve("runtime"))
        val executable = runtimeRoot.resolve("bin/sdl-window")
        Files.createDirectories(executable.parent)
        Files.write(executable, byteArrayOf(2))
        Files.createDirectories(runtimeRoot.resolve("host"))
        return RuntimeProbeRequest(
            nativeLibraryDir = nativeLibraryDir,
            runtimeRoot = runtimeRoot,
            executable = executable,
            environment = environment,
            executionMode = executionMode,
        )
    }

    private fun nativeProbeRequest(
        environment: Map<String, String> = emptyMap(),
    ): RuntimeProbeRequest {
        val request = validRequest(
            environment = environment,
            executionMode = RuntimeProbeExecutionMode.HOST_GLIBC_NATIVE,
        )
        val executable = request.runtimeRoot.resolve("host/fexcore-smoke")
        Files.write(executable, byteArrayOf(5))
        return request.copy(executable = executable)
    }
}
