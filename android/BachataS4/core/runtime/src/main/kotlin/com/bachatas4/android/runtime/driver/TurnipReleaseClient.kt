package com.bachatas4.android.runtime.driver

import java.net.HttpURLConnection
import java.net.URI
import java.net.URL
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.Files
import java.nio.file.Path
import java.nio.file.StandardCopyOption
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

private fun InputStream.readLimited(limit: Int): ByteArray {
    val output = ByteArrayOutputStream()
    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
    while (output.size() < limit) {
        val count = read(buffer, 0, minOf(buffer.size, limit - output.size()))
        if (count < 0) break
        output.write(buffer, 0, count)
    }
    return output.toByteArray()
}

data class HttpRequest(val url: String, val headers: Map<String, String> = emptyMap())
data class HttpResponse(val status: Int, val headers: Map<String, String>, val body: ByteArray)
fun interface HttpTransport { fun execute(request: HttpRequest): HttpResponse }

class UrlConnectionHttpTransport : HttpTransport {
    override fun execute(request: HttpRequest): HttpResponse {
        require(URI(request.url).scheme == "https") { "HTTP URL must use HTTPS" }
        val connection = URL(request.url).openConnection() as HttpURLConnection
        return try {
            connection.connectTimeout = TIMEOUT_MS
            connection.readTimeout = TIMEOUT_MS
            connection.instanceFollowRedirects = true
            request.headers.forEach(connection::setRequestProperty)
            val status = connection.responseCode
            val stream = if (status in 200..299) connection.inputStream else connection.errorStream
            val body = stream?.use { it.readLimited(MAX_METADATA_BYTES + 1) } ?: byteArrayOf()
            require(body.size <= MAX_METADATA_BYTES) { "GitHub release metadata exceeds 4 MiB" }
            HttpResponse(
                status,
                connection.headerFields.filterKeys { it != null }.mapValues { it.value.joinToString(",") },
                body,
            )
        } finally {
            connection.disconnect()
        }
    }

    private companion object {
        const val TIMEOUT_MS = 15_000
        const val MAX_METADATA_BYTES = 4 * 1024 * 1024
    }
}

@Serializable
data class TurnipReleaseAsset(
    val releaseTag: String,
    val publishedAt: String,
    val name: String,
    val size: Long,
    val downloadUrl: String,
)

class TurnipReleaseClient(
    private val transport: HttpTransport,
    private val cacheFile: Path,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    fun listReleases(forceRefresh: Boolean = false): List<TurnipReleaseAsset> {
        val cached = loadCache()
        val now = clock()
        if (!forceRefresh && cached != null && now - cached.fetchedAtMs in 0..CACHE_TTL_MS) {
            return cached.assets
        }
        val headers = buildMap {
            put("Accept", "application/vnd.github+json")
            put("User-Agent", "BachataS4")
            cached?.etag?.let { put("If-None-Match", it) }
        }
        return try {
            val response = transport.execute(HttpRequest(API_URL, headers))
            when (response.status) {
                200 -> {
                    val assets = parse(response.body)
                    val updated = CachedReleaseFeed(response.header("ETag"), now, assets)
                    writeCache(updated)
                    assets
                }
                304 -> requireNotNull(cached) { "GitHub returned 304 without cached releases" }.also {
                    writeCache(it.copy(fetchedAtMs = now))
                }.assets
                else -> cached?.assets ?: error("GitHub releases request failed: HTTP ${response.status}")
            }
        } catch (error: IllegalArgumentException) {
            throw error
        } catch (error: Exception) {
            cached?.assets ?: throw error
        }
    }

    private fun parse(bytes: ByteArray): List<TurnipReleaseAsset> {
        val releases = runCatching { json.decodeFromString<List<GitHubRelease>>(bytes.decodeToString()) }
            .getOrElse { throw IllegalArgumentException("GitHub release metadata is invalid", it) }
        return releases.filterNot { it.draft }.flatMap { release ->
            release.assets.filter { it.name.endsWith("-EMULATOR.zip") }.map { asset ->
                require(asset.size in 1..MAX_ASSET_BYTES) { "Turnip release asset size is invalid" }
                requireTrustedAssetUrl(asset.downloadUrl)
                TurnipReleaseAsset(release.tag, release.publishedAt, asset.name, asset.size, asset.downloadUrl)
            }
        }
    }

    private fun loadCache(): CachedReleaseFeed? {
        if (!Files.isRegularFile(cacheFile)) return null
        return runCatching { json.decodeFromString<CachedReleaseFeed>(Files.readAllBytes(cacheFile).decodeToString()) }.getOrNull()
    }

    private fun writeCache(cache: CachedReleaseFeed) {
        Files.createDirectories(requireNotNull(cacheFile.parent) { "Release cache path has no parent" })
        val temporary = cacheFile.resolveSibling("${cacheFile.fileName}.tmp")
        Files.write(temporary, json.encodeToString(cache).encodeToByteArray())
        try {
            Files.move(temporary, cacheFile, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE)
        } catch (_: AtomicMoveNotSupportedException) {
            Files.move(temporary, cacheFile, StandardCopyOption.REPLACE_EXISTING)
        } finally {
            Files.deleteIfExists(temporary)
        }
    }

    private fun HttpResponse.header(name: String): String? =
        headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value

    @Serializable
    private data class CachedReleaseFeed(
        val etag: String? = null,
        val fetchedAtMs: Long,
        val assets: List<TurnipReleaseAsset>,
    )

    @Serializable
    private data class GitHubRelease(
        @SerialName("tag_name") val tag: String,
        @SerialName("published_at") val publishedAt: String,
        val draft: Boolean = false,
        val assets: List<GitHubAsset> = emptyList(),
    )

    @Serializable
    private data class GitHubAsset(
        val name: String,
        val size: Long,
        @SerialName("browser_download_url") val downloadUrl: String,
    )

    companion object {
        const val REPOSITORY = "JICA98/bachata-s4-drivers"
        const val API_URL = "https://api.github.com/repos/$REPOSITORY/releases?per_page=30"
        const val CACHE_TTL_MS = 24L * 60L * 60L * 1000L
        const val MAX_ASSET_BYTES = 32L * 1024L * 1024L
        private val json = Json { ignoreUnknownKeys = true }

        fun requireTrustedAssetUrl(value: String) {
            val uri = runCatching { URI(value) }.getOrElse { throw IllegalArgumentException("Invalid Turnip asset URL", it) }
            require(uri.scheme == "https" && uri.host.equals("github.com", ignoreCase = true)) {
                "Turnip asset URL is not trusted"
            }
            require(uri.path.startsWith("/$REPOSITORY/releases/download/")) { "Turnip asset URL is not from $REPOSITORY" }
        }
    }
}
