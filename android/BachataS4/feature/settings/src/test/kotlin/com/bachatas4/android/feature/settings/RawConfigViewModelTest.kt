package com.bachatas4.android.feature.settings

import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.settings.ProfileScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class RawConfigViewModelTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val dispatcher = UnconfinedTestDispatcher()
    @Before fun setUp() { Dispatchers.setMain(dispatcher) }
    @After fun tearDown() { Dispatchers.resetMain() }

    @Test
    fun invalidDraftDoesNotSaveAndValidDraftPreservesUnknowns() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = RawConfigViewModel(store)
        viewModel.editShadPs4("{")
        viewModel.save()
        assertFalse(viewModel.state.value.valid)
        assertTrue(store.load(ProfileScope.Global).values.isEmpty())

        viewModel.editShadPs4("""{"Future":{"keep":true},"GPU":{"null_gpu":true}}""")
        viewModel.editBox64("BOX64_LOG=1\nBOX64_FUTURE=keep")
        assertTrue(viewModel.validate())
        viewModel.save()
        val saved = store.load(ProfileScope.Global)
        assertTrue(saved.values.isNotEmpty())
        assertTrue("Future" in saved.unknownShadPs4)
        assertTrue(saved.unknownBox64["BOX64_FUTURE"] == "keep")
    }

    @Test
    fun launchOwnedBox64ValueIsRejected() {
        val viewModel = RawConfigViewModel(RuntimeProfileStore(temporaryFolder.root))
        viewModel.editBox64("BOX64_PATH=/tmp")
        assertFalse(viewModel.validate())
        assertTrue(viewModel.state.value.validation.orEmpty().contains("launch-owned"))
    }
}
