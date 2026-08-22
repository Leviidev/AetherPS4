package com.bachatas4.android.feature.library

import com.bachatas4.android.model.Game
import com.bachatas4.android.runtime.input.NavControllerEvent
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.yield
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class LibraryViewModelTest {
    @Test
    fun sortsGamesByTitleThenId() {
        val viewModel = LibraryViewModel()

        viewModel.setGames(
            listOf(
                Game(id = "CUSA3", title = "zeta", relativePath = "games/CUSA3"),
                Game(id = "CUSA2", title = "Alpha", relativePath = "games/CUSA2"),
                Game(id = "CUSA1", title = "alpha", relativePath = "games/CUSA1"),
            ),
        )

        assertEquals(listOf("CUSA1", "CUSA2", "CUSA3"), viewModel.state.value.games.map { it.id })
    }

    @Test
    fun keepsSelectionWhenPresentOtherwiseSelectsFirstSortedGame() {
        val viewModel = LibraryViewModel()

        viewModel.setGames(listOf(game("B", "Beta"), game("A", "Alpha")))
        assertEquals("A", viewModel.state.value.selectedGameId)

        viewModel.selectGame("B")
        viewModel.setGames(listOf(game("B", "Beta"), game("A", "Alpha")))
        assertEquals("B", viewModel.state.value.selectedGameId)
    }

    @Test
    fun sharePressedEmitsToggleOrientationWhenNoDetails() = runTest {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha")))
        val events = mutableListOf<Unit>()
        val job = backgroundScope.launch {
            viewModel.toggleOrientation.collect { events.add(it) }
        }
        yield()
        runCurrent()

        val handled = viewModel.handleNavEvent(NavControllerEvent("share", pressed = true))
        runCurrent()

        assertTrue(handled)
        assertEquals(1, events.size)
        job.cancel()
    }

    @Test
    fun sharePressedDoesNotEmitWhenDetailsOpen() = runTest {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha")))
        viewModel.showDetails("A")
        val events = mutableListOf<Unit>()
        val job = backgroundScope.launch {
            viewModel.toggleOrientation.collect { events.add(it) }
        }
        yield()
        runCurrent()

        val handled = viewModel.handleNavEvent(NavControllerEvent("share", pressed = true))
        runCurrent()

        assertTrue(handled)
        assertEquals(0, events.size)
        job.cancel()
    }

    @Test
    fun shareReleaseIsIgnored() = runTest {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha")))
        val events = mutableListOf<Unit>()
        val job = backgroundScope.launch {
            viewModel.toggleOrientation.collect { events.add(it) }
        }
        yield()
        runCurrent()

        val handled = viewModel.handleNavEvent(NavControllerEvent("share", pressed = false))
        runCurrent()

        assertFalse(handled)
        assertEquals(0, events.size)
        job.cancel()
    }

    @Test
    fun dpadRightStillNavigatesAfterShareWiring() {
        val viewModel = LibraryViewModel()
        viewModel.setGames(listOf(game("A", "Alpha"), game("B", "Beta")))
        assertEquals("A", viewModel.state.value.selectedGameId)

        val handled = viewModel.handleNavEvent(NavControllerEvent("dpad_right", pressed = true))

        assertTrue(handled)
        assertEquals("B", viewModel.state.value.selectedGameId)
    }

    private fun game(id: String, title: String) = Game(id, title, "games/$id")
}
