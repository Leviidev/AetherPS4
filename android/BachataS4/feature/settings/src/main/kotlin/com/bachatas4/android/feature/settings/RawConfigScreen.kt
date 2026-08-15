package com.bachatas4.android.feature.settings

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.runtime.settings.ProfileScope

@Composable
fun RawConfigScreen(
    scope: ProfileScope,
    onBack: () -> Unit,
    viewModel: RawConfigViewModel = hiltViewModel(),
) {
    LaunchedEffect(scope) { viewModel.load(scope) }
    val state by viewModel.state.collectAsState()
    
    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        BachataPanel(
            modifier = Modifier.fillMaxWidth(),
            color = BachataPalette.RaisedSurface
        ) {
            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    text = "Raw Runtime Configuration",
                    style = MaterialTheme.typography.titleMedium,
                    color = BachataPalette.Primary,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    text = "Edit raw JSON and environment variables. Drafts must pass validation before saving.",
                    style = MaterialTheme.typography.bodySmall,
                    color = BachataPalette.Secondary
                )
            }
        }

        OutlinedTextField(
            value = state.shadPs4Json,
            onValueChange = viewModel::editShadPs4,
            label = { Text("shadPS4 JSON") },
            textStyle = MaterialTheme.typography.bodyMedium.copy(
                fontFamily = FontFamily.Monospace,
                color = BachataPalette.Primary
            ),
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
        )

        OutlinedTextField(
            value = state.box64Environment,
            onValueChange = viewModel::editBox64,
            label = { Text("Box64 Environment Variables") },
            textStyle = MaterialTheme.typography.bodyMedium.copy(
                fontFamily = FontFamily.Monospace,
                color = BachataPalette.Primary
            ),
            modifier = Modifier
                .fillMaxWidth()
                .weight(1f),
        )

        state.validation?.let { msg ->
            val panelColor = if (state.valid) Color(0xFF213D21) else Color(0xFF3D2121)
            val textColor = if (state.valid) Color(0xFFD1FFD1) else MaterialTheme.colorScheme.error
            BachataPanel(
                modifier = Modifier.fillMaxWidth(),
                color = panelColor
            ) {
                Text(
                    text = msg,
                    modifier = Modifier.padding(12.dp),
                    color = textColor,
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Spacer(modifier = Modifier.weight(1f))
            BachataPrimaryButton(onClick = { viewModel.validate() }) {
                Text("Validate")
            }
            BachataPrimaryButton(onClick = viewModel::save, enabled = state.valid) {
                Text("Save")
            }
        }
    }
}
