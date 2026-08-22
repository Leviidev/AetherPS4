package com.bachatas4.android.feature.settings.input

import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.designsystem.BachataBanner
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.input.GamepadInputManager
import com.bachatas4.android.runtime.settings.ProfileScope

// Diagram zones in gamepad-nav traversal order.
private val DIAGRAM_CONTROLS = listOf(
    "l2", "l1", "r1", "r2",
    "l3", "dpad_up", "dpad_left", "dpad_right", "dpad_down",
    "share", "ps", "touchpad", "options",
    "triangle", "square", "circle", "cross", "r3",
)

@Composable
fun ControllerMappingScreen(
    scope: ProfileScope,
    onBack: () -> Unit,
    viewModel: ControllerMappingViewModel = hiltViewModel(),
) {
    LaunchedEffect(scope) { viewModel.load(scope) }
    val state by viewModel.state.collectAsState()
    val profile = state.profiles[state.slot]

    var focusedControl by remember { mutableStateOf<String?>(null) }
    var finetuneOpen by rememberSaveable { mutableStateOf(false) }

    // Gamepad navigation: D-pad moves focus across diagram zones, cross captures, circle backs out.
    DisposableEffect(state.slot) {
        GamepadInputManager.registerNavListener { event ->
            if (!event.pressed) return@registerNavListener false
            when (event.control) {
                "dpad_right" -> {
                    val idx = DIAGRAM_CONTROLS.indexOf(focusedControl)
                    focusedControl = DIAGRAM_CONTROLS[(idx + 1).coerceIn(0, DIAGRAM_CONTROLS.lastIndex)]
                    true
                }
                "dpad_left" -> {
                    val idx = DIAGRAM_CONTROLS.indexOf(focusedControl)
                    focusedControl = DIAGRAM_CONTROLS[(idx - 1).coerceIn(0, DIAGRAM_CONTROLS.lastIndex)]
                    true
                }
                "dpad_up" -> { finetuneOpen = false; true }
                "dpad_down" -> { finetuneOpen = true; true }
                "cross" -> {
                    focusedControl?.let { viewModel.capture(it) }
                    true
                }
                "circle" -> { onBack(); true }
                else -> false
            }
        }
        onDispose { GamepadInputManager.unregisterNavListener() }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 4.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        // --- Action bar ---
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            BachataPrimaryButton(onClick = { viewModel.autoMap(useHatDpad = true) }, modifier = Modifier.weight(1f)) {
                Text("Auto-Map")
            }
            BachataPrimaryButton(onClick = { viewModel.captureSequential() }, modifier = Modifier.weight(1f)) {
                Text("Capture All")
            }
            TextButton(onClick = { viewModel.clear() }) { Text("Clear") }
        }

        // --- Capture banner ---
        val captureTarget = state.captureQueue.firstOrNull()
        if (captureTarget != null) {
            val total = state.captureQueue.size
            BachataBanner(
                title = "Press input for: $captureTarget",
                subtitle = if (total > 1) "${total} remaining · Press a button or move a stick" else "Press a button or move a stick",
                containerColor = BachataPalette.Info,
                titleColor = BachataPalette.InfoText,
                subtitleColor = BachataPalette.InfoSubtle,
                actions = { TextButton(onClick = { viewModel.cancelCapture() }) { Text("Cancel") } },
            )
        }

        // --- Conflict banner ---
        val conflict = state.conflict
        if (conflict != null) {
            BachataBanner(
                title = "Input already maps ${conflict.existing}",
                subtitle = "Replace ${conflict.existing} with ${conflict.target}?",
                containerColor = BachataPalette.Warning,
                titleColor = BachataPalette.WarningText,
                subtitleColor = BachataPalette.WarningText,
                actions = {
                    BachataPrimaryButton(onClick = { viewModel.replaceConflict() }) { Text("Replace") }
                    TextButton(onClick = { viewModel.cancelConflict() }) { Text("Cancel") }
                },
            )
        }

        // --- Visual gamepad diagram ---
        GamepadDiagram(
            profile = profile,
            capturingControl = captureTarget,
            focusedControl = focusedControl,
            onCapture = { viewModel.capture(it) },
        )

        // --- Fine-tune section ---
        TextButton(
            onClick = { finetuneOpen = !finetuneOpen },
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (finetuneOpen) "▼ Fine-Tune" else "▶ Fine-Tune", color = BachataPalette.Secondary)
        }

        if (finetuneOpen) {
            FineTunePanel(
                profile = profile,
                onDeadZone = viewModel::setDeadZone,
                onTriggerThreshold = viewModel::setTriggerThreshold,
                onInvert = viewModel::setInvert,
                onVibration = viewModel::setVibration,
                onMotion = viewModel::setMotion,
            )
        }

        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun FineTunePanel(
    profile: com.bachatas4.android.runtime.input.ControllerProfile,
    onDeadZone: (Float) -> Unit,
    onTriggerThreshold: (Float) -> Unit,
    onInvert: (String, Boolean) -> Unit,
    onVibration: (Boolean) -> Unit,
    onMotion: (Boolean) -> Unit,
) {
    BachataPanel(
        modifier = Modifier.fillMaxWidth(),
        color = BachataPalette.Surface,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            // Stick dead zone
            SectionLabel("Stick Dead Zone")
            Slider(
                value = profile.deadZone,
                onValueChange = onDeadZone,
                valueRange = 0f..0.5f,
            )
            Text("%.2f".format(profile.deadZone), color = BachataPalette.Secondary, style = MaterialTheme.typography.labelSmall)

            // Trigger threshold
            SectionLabel("Trigger Threshold")
            Slider(
                value = profile.triggerThreshold,
                onValueChange = onTriggerThreshold,
                valueRange = 0f..1f,
            )
            Text("%.2f".format(profile.triggerThreshold), color = BachataPalette.Secondary, style = MaterialTheme.typography.labelSmall)

            // Axis inversion
            SectionLabel("Invert Axes")
            InvertToggle("Left Stick X", "left_x", profile.invertAxes, onInvert)
            InvertToggle("Left Stick Y", "left_y", profile.invertAxes, onInvert)
            InvertToggle("Right Stick X", "right_x", profile.invertAxes, onInvert)
            InvertToggle("Right Stick Y", "right_y", profile.invertAxes, onInvert)

            // Toggles
            SectionLabel("Other")
            ToggleRow("Vibration", profile.vibrationEnabled, onVibration)
            ToggleRow("Motion Controls", profile.motionEnabled, onMotion)
        }
    }
}

@Composable
private fun SectionLabel(text: String) {
    Text(text, color = BachataPalette.Primary, fontWeight = FontWeight.Bold, style = MaterialTheme.typography.titleSmall)
}

@Composable
private fun InvertToggle(label: String, control: String, invertAxes: Set<String>, onInvert: (String, Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = BachataPalette.Primary)
        Switch(checked = control in invertAxes, onCheckedChange = { onInvert(control, it) })
    }
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = BachataPalette.Primary)
        Switch(checked = checked, onCheckedChange = onChange)
    }
}
