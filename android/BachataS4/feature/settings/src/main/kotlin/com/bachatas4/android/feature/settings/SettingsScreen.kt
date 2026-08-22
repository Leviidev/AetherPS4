package com.bachatas4.android.feature.settings

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.DisposableEffect
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.foundation.focusable
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.Create
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Share
import androidx.compose.ui.Modifier
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import com.bachatas4.android.runtime.settings.Box64Preset
import com.bachatas4.android.runtime.settings.ProfileScope
import com.bachatas4.android.runtime.settings.RuntimeSettingSpec
import com.bachatas4.android.runtime.settings.RuntimeGuestBackend
import com.bachatas4.android.runtime.settings.SettingKind
import com.bachatas4.android.feature.settings.input.ControllerMappingScreen
import com.bachatas4.android.feature.settings.input.TouchLayoutEditorScreen
import com.bachatas4.android.feature.drivers.DriverManagerScreen
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import com.bachatas4.android.runtime.session.ManagedSession
import com.bachatas4.android.runtime.session.ManagedSessionState
import com.bachatas4.android.designsystem.BachataActionBar
import com.bachatas4.android.designsystem.BachataPanel
import com.bachatas4.android.designsystem.BachataPrimaryButton
import com.bachatas4.android.designsystem.BachataScreenHeader
import com.bachatas4.android.designsystem.theme.BachataPalette

@Composable
fun SettingsScreen(
    onBack: () -> Unit,
    onOpenDrivers: () -> Unit = {},
    /** Drivers tab visibility in Settings (defaults on; Play still uses bundled-only backend). */
    showDriversTab: Boolean = true,
    initialGameId: String? = null,
    viewModel: SettingsViewModel = hiltViewModel(),
) {
    val state by viewModel.state.collectAsState()
    val sessionState by ManagedSession.state.collectAsState()
    var activeTab by remember { mutableStateOf("Runtime") }
    var isSearchActive by remember { mutableStateOf(false) }
    val context = LocalContext.current
    LaunchedEffect(initialGameId) {
        if (initialGameId != null) viewModel.selectScope(ProfileScope.Game(initialGameId))
    }
    val scope = rememberCoroutineScope()
    val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            val text = withContext(Dispatchers.IO) {
                context.contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() }
            }
            if (text != null) viewModel.importJson(text)
        }
    }
    val exporter = rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            val text = viewModel.exportJson()
            withContext(Dispatchers.IO) { context.contentResolver.openOutputStream(uri)?.writer()?.use { it.write(text) } }
        }
    }
    SettingsContent(
        state = state,
        onBack = onBack,
        showDriversTab = showDriversTab,
        activeTab = activeTab,
        onTabSelected = { activeTab = it },
        isSearchActive = isSearchActive,
        onSearchActiveChange = { isSearchActive = it },
        onRuntime = viewModel::selectRuntime,
        onSearch = viewModel::search,
        onCategory = viewModel::selectCategory,
        onScope = viewModel::selectScope,
        onPreset = viewModel::setPreset,
        onGuestBackend = viewModel::setGuestBackend,
        onValue = viewModel::setText,
        onBoolean = viewModel::setValue,
        onReset = { viewModel.setValue(it, null) },
        onImport = { importer.launch(arrayOf("application/json")) },
        onExport = { exporter.launch("bachata-runtime-profile.json") },
        appliesNextLaunch = sessionState is ManagedSessionState.Running || sessionState is ManagedSessionState.Preparing,
    )
}

@Composable
private fun SettingsContent(
    state: SettingsUiState,
    onBack: () -> Unit,
    showDriversTab: Boolean,
    activeTab: String,
    onTabSelected: (String) -> Unit,
    isSearchActive: Boolean,
    onSearchActiveChange: (Boolean) -> Unit,
    onRuntime: (SettingsRuntime) -> Unit,
    onSearch: (String) -> Unit,
    onCategory: (String?) -> Unit,
    onScope: (ProfileScope) -> Unit,
    onPreset: (Box64Preset) -> Unit,
    onGuestBackend: (RuntimeGuestBackend?) -> Unit,
    onValue: (RuntimeSettingSpec, String) -> Unit,
    onBoolean: (RuntimeSettingSpec, JsonPrimitive) -> Unit,
    onReset: (RuntimeSettingSpec) -> Unit,
    onImport: () -> Unit,
    onExport: () -> Unit,
    appliesNextLaunch: Boolean,
) {
    val title = when (val scope = state.scope) {
        ProfileScope.Global -> "Settings"
        is ProfileScope.Game -> "Settings (Game: ${scope.gameId})"
    }
    val preset = state.profile.box64Preset ?: Box64Preset.DEFAULT
    val gameScope = state.scope is ProfileScope.Game
    val selectedGuestBackend = state.profile.guestBackend
    // Box64 flags only when guest backend is explicitly Box64 (global null → FEX default).
    val box64Selected = if (gameScope) {
        selectedGuestBackend == RuntimeGuestBackend.BOX64
    } else {
        (selectedGuestBackend ?: RuntimeGuestBackend.FEX) == RuntimeGuestBackend.BOX64
    }
    val shown = when {
        state.runtime != SettingsRuntime.BOX64 -> state.settings
        !box64Selected -> emptyList()
        preset != Box64Preset.CUSTOM -> emptyList()
        else -> state.settings
    }
    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
    val isLandscape = maxWidth > maxHeight
    Scaffold(
        containerColor = BachataPalette.Canvas,
        bottomBar = {
            if (!isLandscape) {
                BachataActionBar("A  CONFIRM", "↔  CHANGE", "B  BACK")
            }
        },
    ) { contentPadding ->
            val destinationTabs: List<Pair<String, ImageVector>> = remember(showDriversTab) {
                buildList {
                    add(Pair("Runtime", Icons.Default.PlayArrow))
                    add(Pair("CPU", Icons.Default.Build))
                    if (showDriversTab) add(Pair("Drivers", Icons.Default.Settings))
                    add(Pair("RAW", Icons.Default.Info))
                    add(Pair("Controllers", Icons.Default.List))
                    add(Pair("Touch Input", Icons.Default.Create))
                }
            }
            val tabs: List<Pair<String, ImageVector>> = remember(destinationTabs) {
                destinationTabs + listOf(
                    Pair("Import", Icons.Default.Add),
                    Pair("Export", Icons.Default.Share),
                )
            }

            var focusArea by remember { mutableStateOf("tabs") }
            var focusedTabIndex by remember { mutableStateOf(tabs.indexOfFirst { pair -> pair.first == activeTab }.coerceAtLeast(0)) }

            val categoriesList = remember(state.categories) {
                listOf("All") + state.categories
            }
            var focusedCategoryIndex by remember { mutableStateOf(0) }

            LaunchedEffect(activeTab) {
                val idx = tabs.indexOfFirst { pair -> pair.first == activeTab }
                if (idx >= 0 && focusArea == "tabs") {
                    focusedTabIndex = idx
                }
            }

            LaunchedEffect(state.selectedCategory, state.categories) {
                val idx = if (state.selectedCategory == null) 0 else categoriesList.indexOf(state.selectedCategory).coerceAtLeast(0)
                if (focusArea == "categories") {
                    focusedCategoryIndex = idx
                }
            }

            val contentFocusRequester = remember { FocusRequester() }

            val scrollState = rememberScrollState()
            LaunchedEffect(focusedTabIndex) {
                val targetScroll = (focusedTabIndex * 126).coerceAtLeast(0)
                scrollState.animateScrollTo(targetScroll)
            }

            val categoryScrollState = rememberScrollState()
            LaunchedEffect(focusedCategoryIndex) {
                val targetScroll = (focusedCategoryIndex * 90).coerceAtLeast(0)
                categoryScrollState.animateScrollTo(targetScroll)
            }

            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(contentPadding),
            ) {
                DisposableEffect(activeTab, focusArea, focusedTabIndex, focusedCategoryIndex, categoriesList, isLandscape) {
                    com.bachatas4.android.runtime.input.GamepadInputManager.registerNavListener { event ->
                        if (!event.pressed) return@registerNavListener false
                        when (mapSettingsNav(focusArea, event.control, isLandscape)) {
                            SettingsNavIntent.PrevItem -> {
                                when (focusArea) {
                                    "tabs" -> focusedTabIndex = (focusedTabIndex - 1).coerceAtLeast(0)
                                    "categories" -> focusedCategoryIndex = (focusedCategoryIndex - 1).coerceAtLeast(0)
                                }
                                true
                            }
                            SettingsNavIntent.NextItem -> {
                                when (focusArea) {
                                    "tabs" -> focusedTabIndex = (focusedTabIndex + 1).coerceAtMost(tabs.lastIndex)
                                    "categories" -> focusedCategoryIndex = (focusedCategoryIndex + 1).coerceAtMost(categoriesList.lastIndex)
                                }
                                true
                            }
                            SettingsNavIntent.Activate -> {
                                if (focusArea == "tabs") {
                                    val tabName = tabs[focusedTabIndex].first
                                    when (tabName) {
                                        "Import" -> onImport()
                                        "Export" -> onExport()
                                        else -> {
                                            onTabSelected(tabName)
                                            if (tabName == "Runtime") onRuntime(SettingsRuntime.SHADPS4)
                                            if (tabName == "CPU") onRuntime(SettingsRuntime.BOX64)
                                        }
                                    }
                                } else if (focusArea == "categories") {
                                    onCategory(if (focusedCategoryIndex == 0) null else categoriesList[focusedCategoryIndex])
                                }
                                true
                            }
                            SettingsNavIntent.Open -> {
                                if (focusArea == "tabs") {
                                    val tabName = tabs[focusedTabIndex].first
                                    when (tabName) {
                                        "Import" -> onImport()
                                        "Export" -> onExport()
                                        else -> {
                                            onTabSelected(tabName)
                                            if (tabName == "Runtime") onRuntime(SettingsRuntime.SHADPS4)
                                            if (tabName == "CPU") onRuntime(SettingsRuntime.BOX64)
                                            focusArea = if (tabName == "Runtime" || tabName == "CPU") "categories" else "content"
                                            if (focusArea == "content") contentFocusRequester.requestFocus()
                                        }
                                    }
                                } else if (focusArea == "categories") {
                                    focusArea = "content"
                                    contentFocusRequester.requestFocus()
                                }
                                true
                            }
                            SettingsNavIntent.LeaveOrParent -> {
                                when (focusArea) {
                                    "tabs" -> onBack()
                                    "categories" -> focusArea = "tabs"
                                    "content" -> {
                                        focusArea = if (activeTab == "Runtime" || activeTab == "CPU") "categories" else "tabs"
                                    }
                                }
                                true
                            }
                            SettingsNavIntent.Ignore -> false
                        }
                    }
                    onDispose {
                        com.bachatas4.android.runtime.input.GamepadInputManager.unregisterNavListener()
                    }
                }
                if (isLandscape) {
                    Row(modifier = Modifier.fillMaxSize()) {
                        SettingsRail(
                            title = title,
                            isSearchActive = isSearchActive,
                            query = state.query,
                            destinationTabs = destinationTabs,
                            activeTab = activeTab,
                            focusArea = focusArea,
                            focusedTabIndex = focusedTabIndex,
                            onBack = onBack,
                            onSearchActiveChange = onSearchActiveChange,
                            onSearch = onSearch,
                            onDestinationClick = { index, label ->
                                focusedTabIndex = index
                                onTabSelected(label)
                                if (label == "Runtime") onRuntime(SettingsRuntime.SHADPS4)
                                if (label == "CPU") onRuntime(SettingsRuntime.BOX64)
                            },
                            onImport = {
                                focusedTabIndex = destinationTabs.size
                                onImport()
                            },
                            onExport = {
                                focusedTabIndex = destinationTabs.size + 1
                                onExport()
                            },
                        )
                        SettingsTabBody(
                            state = state,
                            activeTab = activeTab,
                            showDriversTab = showDriversTab,
                            shown = shown,
                            categoriesList = categoriesList,
                            focusArea = focusArea,
                            focusedCategoryIndex = focusedCategoryIndex,
                            categoryScrollState = categoryScrollState,
                            box64Selected = box64Selected,
                            gameScope = gameScope,
                            preset = preset,
                            selectedGuestBackend = selectedGuestBackend,
                            appliesNextLaunch = appliesNextLaunch,
                            onTabSelected = onTabSelected,
                            onCategory = onCategory,
                            onPreset = onPreset,
                            onGuestBackend = onGuestBackend,
                            onValue = onValue,
                            onBoolean = onBoolean,
                            onReset = onReset,
                            onFocusedCategoryIndex = { focusedCategoryIndex = it },
                            modifier = Modifier
                                .weight(1f)
                                .fillMaxHeight()
                                .focusRequester(contentFocusRequester)
                                .focusable(),
                        )
                    }
                } else {
                    Column(
                        modifier = Modifier.fillMaxSize(),
                        verticalArrangement = Arrangement.spacedBy(12.dp),
                    ) {
                        if (isSearchActive) {
                            Row(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(horizontal = 16.dp, vertical = 12.dp),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(12.dp)
                            ) {
                                TextButton(onClick = {
                                    onSearchActiveChange(false)
                                    onSearch("")
                                }) {
                                    Text("‹", style = MaterialTheme.typography.headlineMedium)
                                }
                                OutlinedTextField(
                                    value = state.query,
                                    onValueChange = onSearch,
                                    placeholder = { Text("Search settings...") },
                                    modifier = Modifier.weight(1f),
                                    singleLine = true,
                                    trailingIcon = {
                                        TextButton(onClick = { onSearch("") }) { Text("X") }
                                    }
                                )
                            }
                        } else {
                            BachataScreenHeader(
                                title = title,
                                onBack = onBack,
                                actions = {
                                    androidx.compose.material3.Surface(
                                        color = BachataPalette.RaisedSurface,
                                        shape = androidx.compose.foundation.shape.RoundedCornerShape(999.dp),
                                        modifier = Modifier.clickable { onSearchActiveChange(true) }
                                    ) {
                                        Text(
                                            text = "SEARCH",
                                            modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                                            style = MaterialTheme.typography.labelMedium,
                                            color = BachataPalette.Accent,
                                            fontWeight = FontWeight.Bold
                                        )
                                    }
                                }
                            )
                        }
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .horizontalScroll(scrollState)
                                .padding(horizontal = 16.dp),
                            horizontalArrangement = Arrangement.spacedBy(16.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            tabs.forEachIndexed { index, pair ->
                                val label = pair.first
                                val icon = pair.second
                                val isSelected = activeTab == label
                                val isFocused = focusArea == "tabs" && focusedTabIndex == index
                                TopTab(
                                    label = label,
                                    icon = icon,
                                    selected = isSelected,
                                    focused = isFocused,
                                    onClick = {
                                        focusedTabIndex = index
                                        if (label == "Import") {
                                            onImport()
                                        } else if (label == "Export") {
                                            onExport()
                                        } else {
                                            onTabSelected(label)
                                            if (label == "Runtime") onRuntime(SettingsRuntime.SHADPS4)
                                            if (label == "CPU") onRuntime(SettingsRuntime.BOX64)
                                        }
                                    }
                                )
                            }
                        }
                        SettingsTabBody(
                            state = state,
                            activeTab = activeTab,
                            showDriversTab = showDriversTab,
                            shown = shown,
                            categoriesList = categoriesList,
                            focusArea = focusArea,
                            focusedCategoryIndex = focusedCategoryIndex,
                            categoryScrollState = categoryScrollState,
                            box64Selected = box64Selected,
                            gameScope = gameScope,
                            preset = preset,
                            selectedGuestBackend = selectedGuestBackend,
                            appliesNextLaunch = appliesNextLaunch,
                            onTabSelected = onTabSelected,
                            onCategory = onCategory,
                            onPreset = onPreset,
                            onGuestBackend = onGuestBackend,
                            onValue = onValue,
                            onBoolean = onBoolean,
                            onReset = onReset,
                            onFocusedCategoryIndex = { focusedCategoryIndex = it },
                            modifier = Modifier
                                .fillMaxWidth()
                                .weight(1f)
                                .focusRequester(contentFocusRequester)
                                .focusable(),
                        )
                    }
                }
            }
    }
    }
}

@Composable
private fun SettingsTabBody(
    state: SettingsUiState,
    activeTab: String,
    showDriversTab: Boolean,
    shown: List<RuntimeSettingSpec>,
    categoriesList: List<String>,
    focusArea: String,
    focusedCategoryIndex: Int,
    categoryScrollState: androidx.compose.foundation.ScrollState,
    box64Selected: Boolean,
    gameScope: Boolean,
    preset: Box64Preset,
    selectedGuestBackend: RuntimeGuestBackend?,
    appliesNextLaunch: Boolean,
    onTabSelected: (String) -> Unit,
    onCategory: (String?) -> Unit,
    onPreset: (Box64Preset) -> Unit,
    onGuestBackend: (RuntimeGuestBackend?) -> Unit,
    onValue: (RuntimeSettingSpec, String) -> Unit,
    onBoolean: (RuntimeSettingSpec, JsonPrimitive) -> Unit,
    onReset: (RuntimeSettingSpec) -> Unit,
    onFocusedCategoryIndex: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Box(modifier = modifier) {
        if (activeTab == "Runtime" || activeTab == "CPU") {
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(bottom = 16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                if (state.runtime != SettingsRuntime.BOX64 || box64Selected) {
                    item {
                        Row(
                            modifier = Modifier
                                .fillMaxWidth()
                                .horizontalScroll(categoryScrollState)
                                .padding(horizontal = 16.dp),
                            horizontalArrangement = Arrangement.spacedBy(8.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            categoriesList.forEachIndexed { index, category ->
                                val isSelected = if (category == "All") state.selectedCategory == null else state.selectedCategory == category
                                val isFocused = focusArea == "categories" && focusedCategoryIndex == index
                                CategoryTab(
                                    label = category,
                                    selected = isSelected,
                                    focused = isFocused,
                                    onClick = {
                                        onFocusedCategoryIndex(index)
                                        onCategory(if (category == "All") null else category)
                                    }
                                )
                            }
                        }
                    }
                }

                if (appliesNextLaunch) {
                    item {
                        BachataPanel(modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(), color = Color(0xFF3A2B18)) {
                            Text("Emulation is active; changes apply next launch.", modifier = Modifier.padding(12.dp), color = Color(0xFFFFDDB5))
                        }
                    }
                }
                if (state.runtime == SettingsRuntime.BOX64) {
                    item {
                        val options = if (gameScope) {
                            listOf<RuntimeGuestBackend?>(null) + RuntimeGuestBackend.entries
                        } else {
                            RuntimeGuestBackend.entries.map { it }
                        }
                        BachataPanel(
                            modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
                            color = BachataPalette.RaisedSurface,
                        ) {
                            Column(
                                modifier = Modifier.padding(12.dp),
                                verticalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                Text(
                                    "Guest CPU backend",
                                    fontWeight = FontWeight.SemiBold,
                                    color = BachataPalette.Primary,
                                )
                                LazyRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                    items(options) { option ->
                                        val label = option?.name?.lowercase() ?: "inherit"
                                        val isSelected = if (gameScope) {
                                            selectedGuestBackend == option
                                        } else {
                                            (selectedGuestBackend ?: RuntimeGuestBackend.FEX) == option
                                        }
                                        CategoryTab(label, isSelected, focused = false) {
                                            onGuestBackend(option)
                                        }
                                    }
                                }
                                Text(
                                    if (gameScope && selectedGuestBackend == null) {
                                        "Uses the global backend."
                                    } else {
                                        "FEX is the default; Box64 remains an explicit compatibility fallback."
                                    },
                                    color = BachataPalette.Secondary,
                                )
                            }
                        }
                    }
                }
                if (state.runtime == SettingsRuntime.BOX64 && box64Selected) {
                    item {
                        BachataPanel(modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(), color = BachataPalette.RaisedSurface) {
                            Column(modifier = Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                                Text("Official Box64 preset", fontWeight = FontWeight.SemiBold, color = BachataPalette.Primary)
                                LazyRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                                    items(Box64Preset.entries) { option ->
                                        CategoryTab(option.name.lowercase(), preset == option, focused = false) { onPreset(option) }
                                    }
                                }
                                if (preset != Box64Preset.CUSTOM) {
                                    Text(
                                        "${preset.name.lowercase()} uses upstream Box64 defaults. Select custom to fine-tune all flags.",
                                        color = BachataPalette.Secondary,
                                    )
                                }
                            }
                        }
                    }
                }
                state.error?.let { message ->
                    item { Text(message, modifier = Modifier.padding(horizontal = 16.dp), color = MaterialTheme.colorScheme.error) }
                }
                if (shown.isEmpty() && state.query.isNotEmpty()) {
                    item {
                        Box(
                            modifier = Modifier.fillMaxWidth().padding(32.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                text = "No Results for \"${state.query}\"",
                                color = BachataPalette.Secondary,
                                style = MaterialTheme.typography.bodyLarge
                            )
                        }
                    }
                }
                items(shown, key = { it.id }) { spec ->
                    SettingEditor(spec, state, onValue, onBoolean, onReset)
                }
            }
        } else {
            when (activeTab) {
                "Drivers" -> {
                    if (showDriversTab) {
                        DriverManagerScreen(
                            scope = state.scope,
                            onBack = { onTabSelected("Runtime") },
                            standalone = false,
                        )
                    }
                }
                "RAW" -> {
                    RawConfigScreen(
                        scope = state.scope,
                        onBack = { onTabSelected("Runtime") }
                    )
                }
                "Controllers" -> {
                    ControllerMappingScreen(
                        scope = state.scope,
                        onBack = { onTabSelected("Runtime") }
                    )
                }
                "Touch Input" -> {
                    TouchLayoutEditorScreen(
                        scope = state.scope,
                        onBack = { onTabSelected("Runtime") }
                    )
                }
            }
        }
    }
}

@Composable
private fun SettingsRailItem(
    label: String,
    icon: ImageVector,
    selected: Boolean,
    focused: Boolean,
    onClick: () -> Unit,
) {
    androidx.compose.material3.Surface(
        onClick = onClick,
        shape = androidx.compose.foundation.shape.RoundedCornerShape(10.dp),
        color = if (selected) BachataPalette.RaisedSurface else Color.Transparent,
        border = androidx.compose.foundation.BorderStroke(
            width = 2.dp,
            color = when {
                focused -> BachataPalette.Accent
                selected -> BachataPalette.Accent.copy(alpha = 0.5f)
                else -> Color.Transparent
            },
        ),
        modifier = Modifier.fillMaxWidth(),
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            androidx.compose.material3.Icon(
                imageVector = icon,
                contentDescription = label,
                tint = if (selected || focused) BachataPalette.Accent else BachataPalette.Secondary,
                modifier = Modifier.size(20.dp),
            )
            Text(
                text = label,
                color = if (selected || focused) BachataPalette.Primary else BachataPalette.Secondary,
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = if (selected || focused) FontWeight.Bold else FontWeight.Normal,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun SettingsRail(
    title: String,
    isSearchActive: Boolean,
    query: String,
    destinationTabs: List<Pair<String, ImageVector>>,
    activeTab: String,
    focusArea: String,
    focusedTabIndex: Int,
    onBack: () -> Unit,
    onSearchActiveChange: (Boolean) -> Unit,
    onSearch: (String) -> Unit,
    onDestinationClick: (index: Int, label: String) -> Unit,
    onImport: () -> Unit,
    onExport: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val importIndex = destinationTabs.size
    val exportIndex = destinationTabs.size + 1
    val railScroll = rememberScrollState()
    LaunchedEffect(focusedTabIndex) {
        val itemPx = 56
        railScroll.animateScrollTo((focusedTabIndex * itemPx).coerceAtLeast(0))
    }
    Column(
        modifier = modifier
            .width(280.dp)
            .fillMaxHeight()
            .background(BachataPalette.Surface)
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (isSearchActive) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                TextButton(onClick = {
                    onSearchActiveChange(false)
                    onSearch("")
                }) {
                    Text("‹", style = MaterialTheme.typography.headlineMedium)
                }
                OutlinedTextField(
                    value = query,
                    onValueChange = onSearch,
                    placeholder = { Text("Search settings...") },
                    modifier = Modifier.weight(1f),
                    singleLine = true,
                    trailingIcon = {
                        TextButton(onClick = { onSearch("") }) { Text("X") }
                    },
                )
            }
        } else {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                TextButton(onClick = onBack) {
                    Text("‹", style = MaterialTheme.typography.headlineMedium)
                }
                Text(
                    text = title,
                    modifier = Modifier.weight(1f),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                    color = BachataPalette.Primary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                androidx.compose.material3.Surface(
                    color = BachataPalette.RaisedSurface,
                    shape = androidx.compose.foundation.shape.RoundedCornerShape(999.dp),
                    modifier = Modifier.clickable { onSearchActiveChange(true) },
                ) {
                    Text(
                        text = "SEARCH",
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                        style = MaterialTheme.typography.labelMedium,
                        color = BachataPalette.Accent,
                        fontWeight = FontWeight.Bold,
                    )
                }
            }
        }
        Column(
            modifier = Modifier
                .weight(1f)
                .verticalScroll(railScroll),
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            destinationTabs.forEachIndexed { index, pair ->
                SettingsRailItem(
                    label = pair.first,
                    icon = pair.second,
                    selected = activeTab == pair.first,
                    focused = focusArea == "tabs" && focusedTabIndex == index,
                    onClick = { onDestinationClick(index, pair.first) },
                )
            }
            SettingsRailItem(
                label = "Import",
                icon = Icons.Default.Add,
                selected = false,
                focused = focusArea == "tabs" && focusedTabIndex == importIndex,
                onClick = onImport,
            )
            SettingsRailItem(
                label = "Export",
                icon = Icons.Default.Share,
                selected = false,
                focused = focusArea == "tabs" && focusedTabIndex == exportIndex,
                onClick = onExport,
            )
        }
    }
}

@Composable
private fun TopTab(
    label: String,
    icon: androidx.compose.ui.graphics.vector.ImageVector,
    selected: Boolean,
    focused: Boolean,
    onClick: () -> Unit,
) {
    androidx.compose.material3.Surface(
        onClick = onClick,
        shape = androidx.compose.foundation.shape.RoundedCornerShape(10.dp),
        color = if (selected) BachataPalette.RaisedSurface else BachataPalette.Surface,
        border = androidx.compose.foundation.BorderStroke(
            width = 2.dp,
            color = when {
                focused -> BachataPalette.Accent
                selected -> BachataPalette.Accent.copy(alpha = 0.5f)
                else -> Color.Transparent
            }
        ),
        modifier = Modifier
            .width(110.dp)
            .height(76.dp)
    ) {
        Column(
            modifier = Modifier.fillMaxSize().padding(8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            androidx.compose.material3.Icon(
                imageVector = icon,
                contentDescription = label,
                tint = if (selected || focused) BachataPalette.Accent else BachataPalette.Secondary,
                modifier = Modifier.size(24.dp)
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                text = label,
                color = if (selected || focused) BachataPalette.Primary else BachataPalette.Secondary,
                style = MaterialTheme.typography.labelSmall,
                fontWeight = if (selected || focused) FontWeight.Bold else FontWeight.Normal,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Composable
private fun CategoryTab(
    label: String,
    selected: Boolean,
    focused: Boolean,
    onClick: () -> Unit,
) {
    androidx.compose.material3.Surface(
        onClick = onClick,
        shape = androidx.compose.foundation.shape.RoundedCornerShape(999.dp),
        color = if (selected) BachataPalette.RaisedSurface else BachataPalette.Surface,
        border = androidx.compose.foundation.BorderStroke(
            width = 2.dp,
            color = when {
                focused -> BachataPalette.Accent
                selected -> BachataPalette.Accent.copy(alpha = 0.4f)
                else -> Color.Transparent
            }
        ),
        modifier = Modifier
            .padding(vertical = 4.dp)
    ) {
        Text(
            text = label,
            color = if (selected || focused) BachataPalette.Primary else BachataPalette.Secondary,
            style = MaterialTheme.typography.labelMedium,
            fontWeight = if (selected || focused) FontWeight.Bold else FontWeight.Normal,
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 8.dp)
        )
    }
}

@Composable
private fun HighlightedText(
    text: String,
    query: String,
    modifier: Modifier = Modifier,
    style: androidx.compose.ui.text.TextStyle = androidx.compose.ui.text.TextStyle.Default,
    color: Color = BachataPalette.Primary
) {
    if (query.isBlank() || !text.contains(query, ignoreCase = true)) {
        Text(text = text, modifier = modifier, style = style, color = color)
        return
    }
    val annotatedString = remember(text, query) {
        androidx.compose.ui.text.buildAnnotatedString {
            var startIndex = 0
            while (true) {
                val index = text.indexOf(query, startIndex, ignoreCase = true)
                if (index == -1) {
                    append(text.substring(startIndex))
                    break
                }
                append(text.substring(startIndex, index))
                pushStyle(androidx.compose.ui.text.SpanStyle(background = BachataPalette.Accent, color = BachataPalette.OnAccent))
                append(text.substring(index, index + query.length))
                pop()
                startIndex = index + query.length
            }
        }
    }
    Text(text = annotatedString, modifier = modifier, style = style, color = color)
}

/**
 * Resolve stored enum value for display/selection.
 * Unknown or missing values fall back to catalog default, then first choice.
 */
private fun resolveEnumChoice(spec: RuntimeSettingSpec, stored: JsonElement?): String {
    val content = (stored as? JsonPrimitive)?.content
    if (content != null && content in spec.choices) return content
    val defaultContent = (spec.defaultValue as? JsonPrimitive)?.content
    if (defaultContent != null && defaultContent in spec.choices) return defaultContent
    return spec.choices.firstOrNull().orEmpty()
}

@Composable
private fun SettingEditorHeader(
    spec: RuntimeSettingSpec,
    state: SettingsUiState,
    inherited: Boolean,
) {
    HighlightedText(
        text = spec.title.ifBlank { spec.nativeKey },
        query = state.query,
        style = MaterialTheme.typography.titleMedium,
        color = BachataPalette.Primary,
    )
    Row {
        HighlightedText(
            text = spec.category,
            query = state.query,
            style = MaterialTheme.typography.labelSmall,
            color = BachataPalette.Secondary,
        )
        Text(" · ", style = MaterialTheme.typography.labelSmall, color = BachataPalette.Secondary)
        HighlightedText(
            text = spec.nativeKey,
            query = state.query,
            style = MaterialTheme.typography.labelSmall,
            color = BachataPalette.Secondary,
        )
        if (inherited) {
            Text(" · inherited", style = MaterialTheme.typography.labelSmall, color = BachataPalette.Secondary)
        }
    }
    if (spec.help.isNotBlank()) {
        HighlightedText(
            text = spec.help,
            query = state.query,
            style = MaterialTheme.typography.bodySmall,
            color = BachataPalette.Secondary,
        )
    }
    spec.readOnlyReason?.let {
        Text("Managed: $it", color = Color(0xFFFFDDB5), style = MaterialTheme.typography.bodySmall)
    }
}

@Composable
private fun SettingEditor(
    spec: RuntimeSettingSpec,
    state: SettingsUiState,
    onValue: (RuntimeSettingSpec, String) -> Unit,
    onBoolean: (RuntimeSettingSpec, JsonPrimitive) -> Unit,
    onReset: (RuntimeSettingSpec) -> Unit,
) {
    val stored = state.profile.values[spec.id]
    val current = stored ?: spec.defaultValue
    val inherited = state.scope is ProfileScope.Game && spec.id !in state.profile.values
    val isChoiceSetting = spec.kind == SettingKind.ENUM || spec.choices.isNotEmpty()
    var text by remember(spec.id, current) {
        mutableStateOf(
            if (current is JsonArray) {
                current.joinToString(",") { (it as JsonPrimitive).content }
            } else {
                (current as? JsonPrimitive)?.content.orEmpty()
            },
        )
    }
    BachataPanel(
        modifier = Modifier.padding(horizontal = 16.dp).fillMaxWidth(),
        color = if (inherited) BachataPalette.RaisedSurface else BachataPalette.Surface,
    ) {
        when {
            spec.kind == SettingKind.BOOLEAN -> {
                Row(
                    modifier = Modifier.fillMaxWidth().padding(16.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        SettingEditorHeader(spec, state, inherited)
                    }
                    val checked = (current as? JsonPrimitive)?.booleanOrNull ?: false
                    Switch(checked, { onBoolean(spec, JsonPrimitive(it)) }, enabled = spec.readOnlyReason == null)
                }
            }
            isChoiceSetting -> {
                val selected = resolveEnumChoice(spec, stored ?: current)
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    SettingEditorHeader(spec, state, inherited)
                    LazyRow(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                        items(spec.choices) { choice ->
                            CategoryTab(
                                label = choice,
                                selected = selected == choice,
                                focused = false,
                                onClick = {
                                    if (spec.readOnlyReason == null) onValue(spec, choice)
                                },
                            )
                        }
                    }
                    TextButton(onClick = { onReset(spec) }, enabled = spec.readOnlyReason == null) {
                        Text(if (state.scope is ProfileScope.Game) "Inherit" else "Default")
                    }
                }
            }
            else -> {
                Column(
                    Modifier.fillMaxWidth().padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    SettingEditorHeader(spec, state, inherited)
                    OutlinedTextField(
                        value = text,
                        onValueChange = { text = it },
                        modifier = Modifier.fillMaxWidth(),
                        enabled = spec.readOnlyReason == null,
                    )
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                        BachataPrimaryButton(onClick = { onValue(spec, text) }, enabled = spec.readOnlyReason == null) {
                            Text("Apply")
                        }
                        TextButton(onClick = { onReset(spec) }, enabled = spec.readOnlyReason == null) {
                            Text(if (state.scope is ProfileScope.Game) "Inherit" else "Default")
                        }
                    }
                }
            }
        }
    }
}
