package com.bachatas4.android.runtime.driver

import java.io.IOException
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class TurnipReleaseClientTest {
    @Test
    fun filtersTrustedEmulatorAssetsAndCachesWithEtag() {
        val cache = Files.createTempDirectory("release-cache").resolve("releases.json")
        val requests = mutableListOf<HttpRequest>()
        val transport = HttpTransport { request ->
            requests += request
            HttpResponse(200, mapOf("ETag" to "etag-1"), RELEASE_JSON.toByteArray())
        }
        val client = TurnipReleaseClient(transport, cache, clock = { 1_000L })

        val assets = client.listReleases()

        assertEquals(listOf("Turnip-26-1.1-EMULATOR.zip"), assets.map { it.name })
        assertEquals("31_may_2026", assets.single().releaseTag)
        assertEquals(TurnipReleaseClient.API_URL, requests.single().url)
        assertTrue(Files.isRegularFile(cache))
    }

    @Test
    fun staleCacheUsesConditionalRequestAndSurvivesOfflineFailure() {
        val cache = Files.createTempDirectory("release-cache").resolve("releases.json")
        var now = 1_000L
        var calls = 0
        val transport = HttpTransport { request ->
            calls += 1
            if (calls == 1) HttpResponse(200, mapOf("ETag" to "etag-1"), RELEASE_JSON.toByteArray())
            else {
                assertEquals("etag-1", request.headers["If-None-Match"])
                throw IOException("offline")
            }
        }
        val client = TurnipReleaseClient(transport, cache, clock = { now })
        val initial = client.listReleases()
        now += TurnipReleaseClient.CACHE_TTL_MS + 1

        assertEquals(initial, client.listReleases())
        assertEquals(2, calls)
    }

    @Test
    fun rejectsUntrustedDownloadUrlsWhenNoCacheExists() {
        val cache = Files.createTempDirectory("release-cache").resolve("releases.json")
        val transport = HttpTransport {
            HttpResponse(200, emptyMap(), RELEASE_JSON.replace("https://github.com/", "https://evil.example/").toByteArray())
        }

        assertThrows(IllegalArgumentException::class.java) {
            TurnipReleaseClient(transport, cache).listReleases()
        }
    }

    private companion object {
        val RELEASE_JSON = """
            [{
              "tag_name":"31_may_2026",
              "published_at":"2026-05-30T21:07:14Z",
              "draft":false,
              "assets":[
                {"name":"Turnip-26-1.1-EMULATOR.zip","size":3067702,"browser_download_url":"https://github.com/JICA98/bachata-s4-drivers/releases/download/31_may_2026/Turnip.zip"},
                {"name":"Turnip-26.1.1-MAGISK-KSU.zip","size":3070797,"browser_download_url":"https://github.com/JICA98/bachata-s4-drivers/releases/download/31_may_2026/Magisk.zip"}
              ]
            }]
        """.trimIndent()
    }
}
