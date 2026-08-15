package com.bachatas4.android.feature.settings.input

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.input.ControllerProfile
import com.bachatas4.android.runtime.input.PhysicalBinding

/**
 * The logical PS4 controls laid out in a controller-shaped diagram.
 * Each zone is a clickable card. Tapping a zone enters capture mode for that control.
 *
 * @param profile Current controller profile (read for binding display).
 * @param capturingControl The control currently being captured (highlighted), or null.
 * @param focusedControl The control currently focused by gamepad nav (ring), or null.
 * @param onCapture Called when a zone is tapped.
 */
@Composable
fun GamepadDiagram(
    profile: ControllerProfile,
    capturingControl: String?,
    focusedControl: String?,
    onCapture: (String) -> Unit,
    modifier: Modifier = Modifier,
) {
    BachataPanel(
        modifier = modifier.fillMaxWidth(),
        color = BachataPalette.RaisedSurface,
    ) {
        Column(
            modifier = Modifier.fillMaxWidth().padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            // Shoulder + trigger row
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                DiagramZone("l2", "L2", profile, capturingControl, focusedControl, onCapture, width = 72)
                DiagramZone("l1", "L1", profile, capturingControl, focusedControl, onCapture, width = 72)
                DiagramZone("r1", "R1", profile, capturingControl, focusedControl, onCapture, width = 72)
                DiagramZone("r2", "R2", profile, capturingControl, focusedControl, onCapture, width = 72)
            }

            // Left stick + D-Pad row
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                DiagramZone("l3", "L-Stick\n(L3)", profile, capturingControl, focusedControl, onCapture, width = 100, height = 100, circular = true)
                DpadCluster(profile, capturingControl, focusedControl, onCapture)
            }

            // Share + Options + PS + Touchpad
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceEvenly,
            ) {
                DiagramZone("share", "Share", profile, capturingControl, focusedControl, onCapture, width = 64)
                DiagramZone("ps", "PS", profile, capturingControl, focusedControl, onCapture, width = 64)
                DiagramZone("touchpad", "Touchpad", profile, capturingControl, focusedControl, onCapture, width = 80)
                DiagramZone("options", "Options", profile, capturingControl, focusedControl, onCapture, width = 64)
            }

            // Face buttons + Right stick row
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
            ) {
                FaceButtonCluster(profile, capturingControl, focusedControl, onCapture)
                DiagramZone("r3", "R-Stick\n(R3)", profile, capturingControl, focusedControl, onCapture, width = 100, height = 100, circular = true)
            }
        }
    }
}

@Composable
private fun DpadCluster(
    profile: ControllerProfile,
    capturingControl: String?,
    focusedControl: String?,
    onCapture: (String) -> Unit,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        DiagramZone("dpad_up", "▲", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
        Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
            DiagramZone("dpad_left", "◀", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
            Box(modifier = Modifier.size(40.dp)) { } // center gap
            DiagramZone("dpad_right", "▶", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
        }
        DiagramZone("dpad_down", "▼", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
    }
}

@Composable
private fun FaceButtonCluster(
    profile: ControllerProfile,
    capturingControl: String?,
    focusedControl: String?,
    onCapture: (String) -> Unit,
) {
    Column(horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.spacedBy(2.dp)) {
        DiagramZone("triangle", "△", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
        Row(horizontalArrangement = Arrangement.spacedBy(2.dp)) {
            DiagramZone("square", "□", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
            Box(modifier = Modifier.size(40.dp)) { } // center gap
            DiagramZone("circle", "○", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
        }
        DiagramZone("cross", "✕", profile, capturingControl, focusedControl, onCapture, width = 40, height = 40)
    }
}

@Composable
private fun DiagramZone(
    control: String,
    label: String,
    profile: ControllerProfile,
    capturingControl: String?,
    focusedControl: String?,
    onCapture: (String) -> Unit,
    width: Int = 56,
    height: Int = 56,
    circular: Boolean = false,
) {
    val binding: PhysicalBinding? = profile.bindings[control]
    val isCapturing = capturingControl == control
    val isFocused = focusedControl == control
    val shape = if (circular) CircleShape else RoundedCornerShape(10.dp)

    val borderColor = when {
        isCapturing -> BachataPalette.Accent
        isFocused -> BachataPalette.Primary
        else -> BachataPalette.RaisedSurface
    }
    val borderWidth = if (isCapturing || isFocused) 2.dp else 1.dp
    val bgColor = if (isCapturing) BachataPalette.Accent.copy(alpha = 0.15f) else BachataPalette.Surface

    val bindingText = binding?.displayText() ?: "—"

    Column(
        modifier = Modifier
            .size(width = width.dp, height = height.dp)
            .clip(shape)
            .background(bgColor)
            .border(borderWidth, borderColor, shape)
            .clickable { onCapture(control) }
            .padding(2.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.Center,
    ) {
        Text(
            label,
            color = BachataPalette.Primary,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            textAlign = TextAlign.Center,
        )
        Text(
            bindingText,
            color = if (binding != null) BachataPalette.Accent else BachataPalette.Secondary,
            fontSize = 8.sp,
            textAlign = TextAlign.Center,
            modifier = Modifier.alpha(if (height >= 56) 1f else 0f),
        )
    }
}
