package com.bachatas4.android.runtime.install

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.InterruptedIOException
import java.nio.file.Files
import java.nio.file.Path
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class RuntimeInstallerTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun installsValidZipAndLeavesFilesNonExecutable() {
        val root = runtimeRoot()
        val linker = "linker".encodeToByteArray()
        val config = "BOX64_DYNAREC=1".encodeToByteArray()
        val manifest = manifest(
            version = "1.0.0",
            "lib/linker" to linker,
            "etc/box64.conf" to config,
        )

        val result = RuntimeInstaller(root).install(
            zipOf("lib/linker" to linker, "etc/box64.conf" to config),
            manifest,
        )

        val installed = result.getOrThrow()
        assertArrayEquals(linker, Files.readAllBytes(installed.resolve("lib/linker")))
        assertArrayEquals(config, Files.readAllBytes(installed.resolve("etc/box64.conf")))
        assertFalse(Files.isExecutable(installed.resolve("lib/linker")))
        assertFalse(Files.exists(root.resolve(".staging-1.0.0")))
    }

    @Test
    fun rejectsWrongHash() {
        val root = runtimeRoot()
        val content = "runtime".encodeToByteArray()
        val manifest = RuntimeManifest(
            schemaVersion = 1,
            runtimeVersion = "1.0.0",
            protocolVersion = 1,
            files = listOf(RuntimeFile("runtime.bin", content.size.toLong(), "0".repeat(64))),
        )

        val result = RuntimeInstaller(root).install(zipOf("runtime.bin" to content), manifest)

        assertTrue(result.isFailure)
        assertFalse(Files.exists(root.resolve("1.0.0")))
        assertFalse(Files.exists(root.resolve(".staging-1.0.0")))
    }

    @Test
    fun rejectsZipTraversal() {
        val root = runtimeRoot()
        val content = byteArrayOf(1)
        val manifest = manifest(version = "1.0.0", "../escape" to content)

        val result = RuntimeInstaller(root).install(zipOf("../escape" to content), manifest)

        assertTrue(result.exceptionOrNull() is SecurityException)
        assertFalse(Files.exists(root.parent.resolve("escape")))
        assertFalse(Files.exists(root.resolve(".staging-1.0.0")))
    }

    @Test
    fun rejectsDuplicateNormalizedZipPath() {
        val root = runtimeRoot()
        val content = "same".encodeToByteArray()
        val manifest = manifest(version = "1.0.0", "bin/tool" to content)

        val result = RuntimeInstaller(root).install(
            zipOf("bin/tool" to content, "bin/./tool" to content),
            manifest,
        )

        assertTrue(result.isFailure)
        assertFalse(Files.exists(root.resolve("1.0.0")))
    }

    @Test
    fun rejectsMissingAndUnexpectedEntries() {
        val expected = "expected".encodeToByteArray()

        val missingRoot = runtimeRoot("missing")
        val missing = RuntimeInstaller(missingRoot).install(
            zipOf(),
            manifest(version = "1.0.0", "expected.bin" to expected),
        )
        assertTrue(missing.isFailure)

        val unexpectedRoot = runtimeRoot("unexpected")
        val unexpected = RuntimeInstaller(unexpectedRoot).install(
            zipOf("expected.bin" to expected, "extra.bin" to byteArrayOf(2)),
            manifest(version = "1.0.0", "expected.bin" to expected),
        )
        assertTrue(unexpected.isFailure)
    }

    @Test
    fun interruptedInstallPreservesCurrentVersion() {
        val root = runtimeRoot()
        val currentFile = root.resolve("1.0.0/lib/runtime.so")
        Files.createDirectories(currentFile.parent)
        Files.writeString(currentFile, "current")
        val replacement = "replacement".encodeToByteArray()
        val installer = RuntimeInstaller(root) { _, _ ->
            throw InterruptedIOException("simulated interruption")
        }

        val result = installer.install(
            zipOf("lib/runtime.so" to replacement),
            manifest(version = "2.0.0", "lib/runtime.so" to replacement),
        )

        assertTrue(result.isFailure)
        assertTrue(Files.readString(currentFile) == "current")
        assertFalse(Files.exists(root.resolve("2.0.0")))
        assertFalse(Files.exists(root.resolve(".staging-2.0.0")))
    }

    private fun runtimeRoot(name: String = "runtime"): Path =
        temporaryFolder.newFolder(name).toPath()

    private fun manifest(version: String, vararg files: Pair<String, ByteArray>) = RuntimeManifest(
        schemaVersion = 1,
        runtimeVersion = version,
        protocolVersion = 1,
        files = files.map { (path, content) ->
            RuntimeFile(
                path = path,
                size = content.size.toLong(),
                sha256 = sha256(ByteArrayInputStream(content)),
            )
        },
    )

    private fun zipOf(vararg files: Pair<String, ByteArray>): ByteArrayInputStream {
        val bytes = ByteArrayOutputStream()
        ZipOutputStream(bytes).use { zip ->
            files.forEach { (path, content) ->
                zip.putNextEntry(ZipEntry(path))
                zip.write(content)
                zip.closeEntry()
            }
        }
        return ByteArrayInputStream(bytes.toByteArray())
    }
}
