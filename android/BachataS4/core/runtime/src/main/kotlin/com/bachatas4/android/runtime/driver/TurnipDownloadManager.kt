package com.bachatas4.android.runtime.driver

import java.io.Closeable
import java.io.ByteArrayInputStream
import java.io.FilterInputStream
import java.io.InputStream
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL

data class DownloadResponse(
    val status: Int,
    val contentLength: Long,
    val body: InputStream,
    private val closeAction: () -> Unit = {},
) : Closeable {
    override fun close() {
        body.close()
        closeAction()
    }
}

fun interface DriverAssetTransport { fun open(url: String): DownloadResponse }

class UrlConnectionDriverAssetTransport : DriverAssetTransport {
    override fun open(url: String): DownloadResponse {
        TurnipReleaseClient.requireTrustedAssetUrl(url)
        val connection = URL(url).openConnection() as HttpURLConnection
        connection.connectTimeout = TIMEOUT_MS
        connection.readTimeout = TIMEOUT_MS
        connection.instanceFollowRedirects = true
        connection.setRequestProperty("User-Agent", "BachataS4")
        val status = connection.responseCode
        val body = if (status in 200..299) connection.inputStream else connection.errorStream
            ?: ByteArrayInputStream(byteArrayOf())
        return DownloadResponse(status, connection.contentLengthLong, body, connection::disconnect)
    }

    private companion object { const val TIMEOUT_MS = 15_000 }
}

class TurnipDownloadManager(
    private val installer: TurnipPackageInstaller,
    private val transport: DriverAssetTransport,
) {
    fun download(
        asset: TurnipReleaseAsset,
        onProgress: (copied: Long, total: Long) -> Unit,
    ): InstalledDriver {
        require(asset.name.endsWith("-EMULATOR.zip")) { "Only emulator Turnip assets are supported" }
        require(asset.size in 1..TurnipReleaseClient.MAX_ASSET_BYTES) { "Turnip asset size is invalid" }
        TurnipReleaseClient.requireTrustedAssetUrl(asset.downloadUrl)
        transport.open(asset.downloadUrl).use { response ->
            require(response.status == 200) { "Turnip download failed: HTTP ${response.status}" }
            require(response.contentLength < 0 || response.contentLength == asset.size) {
                "Turnip download length mismatch"
            }
            val checked = ProgressInputStream(response.body, asset.size, onProgress)
            return installer.install(
                checked,
                DriverPackageSource(TurnipReleaseClient.REPOSITORY, asset.releaseTag, asset.name),
            )
        }
    }

    private class ProgressInputStream(
        input: InputStream,
        private val expected: Long,
        private val progress: (Long, Long) -> Unit,
    ) : FilterInputStream(input) {
        private var copied = 0L

        override fun read(): Int {
            checkCancelled()
            val value = super.read()
            if (value < 0) verifyComplete() else record(1)
            return value
        }

        override fun read(buffer: ByteArray, offset: Int, length: Int): Int {
            checkCancelled()
            val count = super.read(buffer, offset, length)
            if (count < 0) verifyComplete() else record(count)
            return count
        }

        private fun record(count: Int) {
            copied += count
            require(copied <= expected) { "Turnip download exceeds expected length" }
            progress(copied, expected)
        }

        private fun verifyComplete() {
            require(copied == expected) { "Turnip download length mismatch" }
        }

        private fun checkCancelled() {
            if (Thread.currentThread().isInterrupted) throw InterruptedIOException("Turnip download cancelled")
        }
    }
}
