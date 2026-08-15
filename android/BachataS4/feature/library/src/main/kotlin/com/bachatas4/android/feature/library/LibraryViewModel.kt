package com.bachatas4.android.feature.library

import androidx.lifecycle.ViewModel
import com.bachatas4.android.model.Game
import com.bachatas4.android.runtime.input.GamepadInputManager
import com.bachatas4.android.runtime.input.NavControllerEvent
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

data class LibraryUiState(
    val games: List<Game> = emptyList(),
    val selectedGameId: String? = null,
    val showDetailsGameId: String? = null,
)

@HiltViewModel
class LibraryViewModel @Inject constructor() : ViewModel() {
    private val mutableState = MutableStateFlow(LibraryUiState())
    val state: StateFlow<LibraryUiState> = mutableState
    private var focusedIndex: Int = 0
    private var numColumns: Int = 1
    private val openSettingsRequest = MutableSharedFlow<String>(extraBufferCapacity = 4)
    val openSettings: SharedFlow<String> = openSettingsRequest
    private val launchRequest = MutableSharedFlow<String>(extraBufferCapacity = 4)
    val launch: SharedFlow<String> = launchRequest
    private val toggleOrientationRequest = MutableSharedFlow<Unit>(extraBufferCapacity = 4)
    val toggleOrientation: SharedFlow<Unit> = toggleOrientationRequest

    fun setGames(games: List<Game>) {
        val sorted = sortGames(games)
        val selected = mutableState.value.selectedGameId
            ?.takeIf { id -> id == "__import_card__" || sorted.any { it.id == id } }
            ?: sorted.firstOrNull()?.id
            ?: "__import_card__"
        mutableState.value = mutableState.value.copy(games = sorted, selectedGameId = selected)
        focusedIndex = if (selected == "__import_card__") sorted.size else sorted.indexOfFirst { it.id == selected }.coerceAtLeast(0)
    }

    fun selectGame(id: String) {
        if (id == "__import_card__") {
            focusedIndex = mutableState.value.games.size
        } else {
            val index = mutableState.value.games.indexOfFirst { it.id == id }
            if (index >= 0) focusedIndex = index
        }
        mutableState.value = mutableState.value.copy(selectedGameId = id)
    }

    fun setNumColumns(columns: Int) {
        numColumns = columns.coerceAtLeast(1)
    }

    fun navigatePrev() = navigate(-1)
    fun navigateNext() = navigate(1)
    fun navigateUp() = navigate(-numColumns)
    fun navigateDown() = navigate(numColumns)

    private fun navigate(direction: Int) {
        val games = mutableState.value.games
        val totalCount = games.size + 1
        if (totalCount <= 0) return
        val isVertical = kotlin.math.abs(direction) > 1
        if (isVertical) {
            val target = focusedIndex + direction
            if (target in 0 until totalCount) {
                focusedIndex = target
            }
        } else {
            focusedIndex = ((focusedIndex + direction) % totalCount + totalCount) % totalCount
        }
        val newSelectedId = if (focusedIndex == games.size) {
            "__import_card__"
        } else {
            games[focusedIndex].id
        }
        mutableState.value = mutableState.value.copy(selectedGameId = newSelectedId)
    }

    fun showDetails(id: String?) {
        mutableState.value = mutableState.value.copy(showDetailsGameId = id)
    }

    fun handleNavEvent(event: NavControllerEvent): Boolean {
        val currentState = mutableState.value
        val detailsId = currentState.showDetailsGameId
        if (detailsId != null) {
            return when {
                event.control == "cross" && event.pressed -> {
                    launchRequest.tryEmit(detailsId)
                    true
                }
                event.control == "circle" && event.pressed -> {
                    showDetails(null)
                    true
                }
                event.control == "square" && event.pressed -> {
                    showDetails(null)
                    openSettingsRequest.tryEmit(detailsId)
                    true
                }
                event.control == "share" && event.pressed -> true
                event.pressed -> true
                else -> false
            }
        }
        return when {
            event.control == "dpad_left" && event.pressed -> {
                navigatePrev()
                true
            }
            event.control == "dpad_right" && event.pressed -> {
                navigateNext()
                true
            }
            event.control == "dpad_up" && event.pressed -> {
                navigateUp()
                true
            }
            event.control == "dpad_down" && event.pressed -> {
                navigateDown()
                true
            }
            event.control == "cross" && event.pressed -> {
                currentState.selectedGameId?.let { launchRequest.tryEmit(it) }
                true
            }
            event.control == "square" && event.pressed -> {
                currentState.selectedGameId?.let { id ->
                    if (id != "__import_card__") {
                        showDetails(id)
                    }
                }
                true
            }
            event.control == "share" && event.pressed -> {
                toggleOrientationRequest.tryEmit(Unit)
                true
            }
            event.control == "circle" && event.pressed -> true
            else -> false
        }
    }

    fun attachNavListener() {
        GamepadInputManager.registerNavListener { event -> handleNavEvent(event) }
    }

    override fun onCleared() {
        GamepadInputManager.unregisterNavListener()
        super.onCleared()
    }

    fun sortGames(games: List<Game>): List<Game> =
        games.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER, Game::title).thenBy(Game::id))
}
