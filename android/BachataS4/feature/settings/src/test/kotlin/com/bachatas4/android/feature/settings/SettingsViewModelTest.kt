package com.bachatas4.android.feature.settings

import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceUntilIdle
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Before
import org.junit.Test
import org.junit.rules.TemporaryFolder

@OptIn(ExperimentalCoroutinesApi::class)
class SettingsViewModelTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val dispatcher = StandardTestDispatcher()

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private fun viewModel() = SettingsViewModel(RuntimeProfileStore(temporaryFolder.root))

    @Test
    fun exposesEveryCatalogEntryAndFiltersByNativeKey() {
        val viewModel = viewModel()
        assertEquals(86, viewModel.state.value.settings.size)
        viewModel.search("BOX64_LOG")
        assertTrue(viewModel.state.value.settings.isEmpty())
        viewModel.selectRuntime(SettingsRuntime.BOX64)
        assertEquals(listOf("BOX64_LOG"), viewModel.state.value.settings.map { it.nativeKey })
    }

    @Test
    fun diagnosticExportHasRevisionsButNoAbsoluteGamePaths() {
        val viewModel = viewModel()
        viewModel.setDiagnostics("runtime-abc123", "vk-uuid", listOf("CUSA00002", "CUSA00001"))
        val export = viewModel.diagnosticExport()
        assertTrue(export.contains("runtimeRevision=runtime-abc123"))
        assertTrue(export.contains("vulkanUuid=vk-uuid"))
        assertTrue(export.contains("gameIds=CUSA00001,CUSA00002"))
        assertFalse(export.contains("/"))
    }

    @Test
    fun selectedCategoryFiltersTheActiveRuntimeWithoutChangingScope() {
        val viewModel = viewModel()

        viewModel.selectCategory("Audio")

        assertTrue(viewModel.state.value.settings.isNotEmpty())
        assertTrue(viewModel.state.value.settings.all { it.category == "Audio" })
        assertEquals(ProfileScope.Global, viewModel.state.value.scope)
    }

    @Test
    fun setsGlobalGuestBackend() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = SettingsViewModel(store)
        advanceUntilIdle()

        viewModel.setGuestBackend(RuntimeGuestBackend.BOX64)
        advanceUntilIdle()

        assertEquals(RuntimeGuestBackend.BOX64, store.load(ProfileScope.Global).guestBackend)
    }

    @Test
    fun clearingGameGuestBackendRestoresInheritance() = runTest(dispatcher) {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = SettingsViewModel(store)
        val scope = ProfileScope.Game("CUSA00900")
        viewModel.selectScope(scope)
        advanceUntilIdle()
        viewModel.setGuestBackend(RuntimeGuestBackend.BOX64)
        advanceUntilIdle()

        viewModel.setGuestBackend(null)
        advanceUntilIdle()

        assertEquals(null, store.load(scope).guestBackend)
    }
}
