package com.bachatas4.android.feature.settings.input

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.bachatas4.android.data.RuntimeProfileStore
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.feature.session.controller.TouchLayout
import com.bachatas4.android.feature.session.controller.TouchLayoutRenderer
import com.bachatas4.android.feature.session.controller.TouchLayoutRepository
import com.bachatas4.android.runtime.settings.ProfileScope
import dagger.hilt.android.lifecycle.HiltViewModel
import javax.inject.Inject
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

data class TouchLayoutEditorUiState(
    val scope: ProfileScope = ProfileScope.Global,
    val layout: TouchLayout = TouchLayout(),
    val selected: String = "left_stick",
    val saved: Boolean = false,
)

@HiltViewModel
class TouchLayoutEditorViewModel @Inject constructor(
    private val profiles: RuntimeProfileStore,
    private val layouts: TouchLayoutRepository,
) : ViewModel() {
    private val mutableState = MutableStateFlow(TouchLayoutEditorUiState())
    val state: StateFlow<TouchLayoutEditorUiState> = mutableState

    fun load(scope: ProfileScope) {
        viewModelScope.launch {
            val id = profiles.load(scope).touchLayoutId
            val base = layouts.load(id)
            val editId = when (scope) { ProfileScope.Global -> "global"; is ProfileScope.Game -> "game-${scope.gameId}" }
            mutableState.value = TouchLayoutEditorUiState(scope, base.copy(id = editId, name = "Custom $editId"))
        }
    }
    fun select(control: String) { mutableState.value = mutableState.value.copy(selected = control) }
    fun move(dx: Float, dy: Float) {
        val p = mutableState.value.layout.controls.first { it.control == mutableState.value.selected }
        update(TouchLayoutRenderer.move(mutableState.value.layout, p.control, p.centerX + dx, p.centerY + dy))
    }
    fun resize(delta: Float) {
        val p = mutableState.value.layout.controls.first { it.control == mutableState.value.selected }
        update(TouchLayoutRenderer.resize(mutableState.value.layout, p.control, p.width + delta, p.height + delta))
    }
    fun visible(value: Boolean) { update(TouchLayoutRenderer.setVisible(mutableState.value.layout, mutableState.value.selected, value)) }
    fun front() { update(TouchLayoutRenderer.bringToFront(mutableState.value.layout, mutableState.value.selected)) }
    fun opacity(value: Float) { update(mutableState.value.layout.copy(opacity = value.coerceIn(0.05f, 1f))) }
    fun scale(value: Float) { update(mutableState.value.layout.copy(scale = value.coerceIn(0.5f, 2f))) }
    fun vibration(value: Boolean) { update(mutableState.value.layout.copy(vibrationEnabled = value)) }
    fun analogCentering(value: Boolean) { update(mutableState.value.layout.copy(analogCentering = value)) }
    fun reset() { update(TouchLayout(id = mutableState.value.layout.id, name = mutableState.value.layout.name)) }
    fun save() {
        layouts.save(mutableState.value.layout)
        viewModelScope.launch {
            profiles.update(mutableState.value.scope) { it.copy(touchLayoutId = mutableState.value.layout.id) }
            mutableState.value = mutableState.value.copy(saved = true)
        }
    }
    fun inherit() {
        viewModelScope.launch { profiles.update(mutableState.value.scope) { it.copy(touchLayoutId = null) } }
    }
    private fun update(layout: TouchLayout) { mutableState.value = mutableState.value.copy(layout = layout, saved = false) }
}

@Composable
fun TouchLayoutEditorScreen(
    scope: ProfileScope,
    onBack: () -> Unit,
    viewModel: TouchLayoutEditorViewModel = hiltViewModel(),
) {
    LaunchedEffect(scope) { viewModel.load(scope) }
    val state by viewModel.state.collectAsState()
    val selected = state.layout.controls.first { it.control == state.selected }
    
    Column(
        modifier = Modifier.fillMaxSize(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        LazyRow(
            modifier = Modifier.fillMaxWidth(),
            contentPadding = PaddingValues(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            items(state.layout.controls, key = { it.control }) { control ->
                val isSelected = state.selected == control.control
                CategoryTab(
                    label = if (control.visible) control.control else "${control.control} (hidden)",
                    selected = isSelected,
                    onClick = { viewModel.select(control.control) }
                )
            }
        }

        LazyColumn(
            modifier = Modifier.fillMaxWidth().weight(1f),
            contentPadding = PaddingValues(bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            item {
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = BachataPalette.Surface
                ) {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = "Control: ${selected.control.uppercase()}",
                            style = MaterialTheme.typography.titleMedium,
                            color = BachataPalette.Primary,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            text = "Position: X = %.2f, Y = %.2f".format(selected.centerX, selected.centerY),
                            style = MaterialTheme.typography.bodySmall,
                            color = BachataPalette.Secondary
                        )
                        
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Visibility", color = BachataPalette.Primary, style = MaterialTheme.typography.bodyMedium)
                            Switch(selected.visible, viewModel::visible)
                        }

                        Row(
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Move: ", color = BachataPalette.Secondary, style = MaterialTheme.typography.bodyMedium)
                            BachataPrimaryButton(onClick = { viewModel.move(-0.02f, 0f) }) { Text("←") }
                            BachataPrimaryButton(onClick = { viewModel.move(0f, -0.02f) }) { Text("↑") }
                            BachataPrimaryButton(onClick = { viewModel.move(0f, 0.02f) }) { Text("↓") }
                            BachataPrimaryButton(onClick = { viewModel.move(0.02f, 0f) }) { Text("→") }
                        }

                        Row(
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Size: ", color = BachataPalette.Secondary, style = MaterialTheme.typography.bodyMedium)
                            BachataPrimaryButton(onClick = { viewModel.resize(-0.02f) }) { Text("Smaller") }
                            BachataPrimaryButton(onClick = { viewModel.resize(0.02f) }) { Text("Larger") }
                            TextButton(onClick = viewModel::front) { Text("Bring to Front") }
                        }
                    }
                }
            }

            item {
                BachataPanel(
                    modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                    color = BachataPalette.Surface
                ) {
                    Column(
                        modifier = Modifier.padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = "Global Preferences",
                            style = MaterialTheme.typography.titleMedium,
                            color = BachataPalette.Primary,
                            fontWeight = FontWeight.Bold
                        )
                        
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Vibration Feedback", color = BachataPalette.Primary, style = MaterialTheme.typography.bodyMedium)
                            Switch(state.layout.vibrationEnabled, viewModel::vibration)
                        }
                        
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Auto-Center Analog Sticks", color = BachataPalette.Primary, style = MaterialTheme.typography.bodyMedium)
                            Switch(state.layout.analogCentering, viewModel::analogCentering)
                        }

                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Opacity", color = BachataPalette.Primary, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                            Text("%.1f".format(state.layout.opacity), color = BachataPalette.Accent, fontWeight = FontWeight.Bold)
                            BachataPrimaryButton(onClick = { viewModel.opacity(state.layout.opacity - 0.1f) }) { Text("-") }
                            BachataPrimaryButton(onClick = { viewModel.opacity(state.layout.opacity + 0.1f) }) { Text("+") }
                        }

                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Text("Layout Scale", color = BachataPalette.Primary, style = MaterialTheme.typography.bodyMedium, modifier = Modifier.weight(1f))
                            Text("%.1f".format(state.layout.scale), color = BachataPalette.Accent, fontWeight = FontWeight.Bold)
                            BachataPrimaryButton(onClick = { viewModel.scale(state.layout.scale - 0.1f) }) { Text("-") }
                            BachataPrimaryButton(onClick = { viewModel.scale(state.layout.scale + 0.1f) }) { Text("+") }
                        }
                    }
                }
            }

            if (state.saved) {
                item {
                    BachataPanel(
                        modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                        color = Color(0xFF213D21)
                    ) {
                        Text(
                            text = "Configuration Saved Successfully",
                            modifier = Modifier.padding(12.dp),
                            color = Color(0xFFD1FFD1),
                            style = MaterialTheme.typography.bodyMedium,
                            fontWeight = FontWeight.Bold
                        )
                    }
                }
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Spacer(modifier = Modifier.weight(1f))
            TextButton(onClick = viewModel::inherit) {
                Text("Inherit Global")
            }
            BachataPrimaryButton(onClick = viewModel::reset) {
                Text("Reset")
            }
            BachataPrimaryButton(onClick = viewModel::save) {
                Text("Save")
            }
        }
    }
}

@Composable
private fun CategoryTab(label: String, selected: Boolean, onClick: () -> Unit) {
    Column(
        modifier = Modifier
            .clickable(onClick = onClick)
            .padding(horizontal = 8.dp, vertical = 6.dp),
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Text(
            text = label,
            color = if (selected) BachataPalette.Primary else BachataPalette.Secondary,
            style = MaterialTheme.typography.bodyMedium,
            fontWeight = if (selected) FontWeight.SemiBold else FontWeight.Normal
        )
        Spacer(modifier = Modifier.height(4.dp))
        Box(
            modifier = Modifier
                .width(32.dp)
                .height(2.dp)
                .background(if (selected) BachataPalette.Accent else Color.Transparent)
        )
    }
}
