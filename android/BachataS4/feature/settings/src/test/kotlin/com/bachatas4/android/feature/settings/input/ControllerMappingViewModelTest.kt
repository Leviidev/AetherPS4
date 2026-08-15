package com.bachatas4.android.feature.settings.input

import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.runtime.input.AxisDirection
import com.bachatas4.android.runtime.input.PhysicalBinding
import com.bachatas4.android.runtime.input.PhysicalBindingKind
import com.bachatas4.android.runtime.settings.ProfileScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ControllerMappingViewModelTest {
    @get:Rule val temporaryFolder = TemporaryFolder()
    private val dispatcher = UnconfinedTestDispatcher()
    @Before fun setUp() { Dispatchers.setMain(dispatcher) }
    @After fun tearDown() { Dispatchers.resetMain() }

    @Test
    fun conflictCanReplaceAndPersistsFourSlots() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        val cross = PhysicalBinding(PhysicalBindingKind.BUTTON, 96)
        viewModel.capture("cross"); viewModel.accept(cross)
        viewModel.capture("circle"); viewModel.accept(cross)
        assertNotNull(viewModel.state.value.conflict)
        viewModel.replaceConflict()
        assertEquals("circle", store.load(ProfileScope.Global).controllerSlots[0].bindings.entries.single().key)
        assertEquals(4, store.load(ProfileScope.Global).controllerSlots.size)
    }

    @Test
    fun perGameInheritanceClearsOverrides() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val scope = ProfileScope.Game("CUSA00001")
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(scope)
        viewModel.autoMap()
        viewModel.inherit()
        assertEquals(emptyList<Any>(), store.load(scope).controllerSlots)
    }

    @Test
    fun autoMapWithHatDpadBindsHATAxesWithDirection() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        viewModel.autoMap(useHatDpad = true)
        val slot0 = viewModel.state.value.profiles[0]
        assertEquals(PhysicalBinding(PhysicalBindingKind.AXIS, 15, AxisDirection.NEGATIVE), slot0.bindings["dpad_left"])
        assertEquals(PhysicalBinding(PhysicalBindingKind.AXIS, 15, AxisDirection.POSITIVE), slot0.bindings["dpad_right"])
        assertEquals(PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.NEGATIVE), slot0.bindings["dpad_up"])
        assertEquals(PhysicalBinding(PhysicalBindingKind.AXIS, 16, AxisDirection.POSITIVE), slot0.bindings["dpad_down"])
    }

    @Test
    fun autoMapWithoutHatBindsButtonKeycodes() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        viewModel.autoMap(useHatDpad = false)
        val slot0 = viewModel.state.value.profiles[0]
        assertEquals(PhysicalBinding(PhysicalBindingKind.BUTTON, 19), slot0.bindings["dpad_up"])
        assertEquals(PhysicalBinding(PhysicalBindingKind.BUTTON, 22), slot0.bindings["dpad_right"])
    }

    @Test
    fun captureSequentialQueuesAllControls() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        viewModel.captureSequential()
        assertTrue(viewModel.state.value.captureQueue.isNotEmpty())
    }

    @Test
    fun cancelCaptureClearsQueue() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        viewModel.capture("cross")
        viewModel.cancelCapture()
        assertNull(viewModel.state.value.captureQueue.firstOrNull())
    }

    @Test
    fun setDeadZonePersists() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        viewModel.setDeadZone(0.25f)
        assertEquals(0.25f, viewModel.state.value.profiles[0].deadZone, 0.001f)
    }

    @Test
    fun selectSlotClampsToBounds() = runBlocking {
        val store = RuntimeProfileStore(temporaryFolder.root)
        val viewModel = ControllerMappingViewModel(store)
        viewModel.load(ProfileScope.Global)
        viewModel.selectSlot(2)
        assertEquals(2, viewModel.state.value.slot)
    }
}
