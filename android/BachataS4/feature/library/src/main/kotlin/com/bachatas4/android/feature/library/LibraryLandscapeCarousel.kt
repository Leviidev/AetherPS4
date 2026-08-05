package com.bachatas4.android.feature.library

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.scale
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.bachatas4.android.designsystem.theme.BachataPalette
import com.bachatas4.android.model.Game

/**
 * Console-style horizontal game carousel for landscape library.
 * Focused cover is larger and centered; neighbors sit dimmed beside it.
 */
@Composable
internal fun LibraryLandscapeCarousel(
    games: List<Game>,
    selectedGameId: String?,
    isImporting: Boolean,
    contentPadding: PaddingValues,
    onSelectGame: (String) -> Unit,
    onShowDetails: (String) -> Unit,
    onImport: () -> Unit,
    header: @Composable () -> Unit,
    importStatus: @Composable () -> Unit,
    modifier: Modifier = Modifier,
) {
    val selected = games.firstOrNull { it.id == selectedGameId }
    val isImportSelected = selectedGameId == "__import_card__"
    val listState = rememberLazyListState()
    val flingBehavior = rememberSnapFlingBehavior(lazyListState = listState)
    val itemCount = games.size + 1
    val selectedIndex = remember(selectedGameId, games) {
        when {
            selectedGameId == "__import_card__" -> games.size
            selectedGameId != null -> games.indexOfFirst { it.id == selectedGameId }.coerceAtLeast(0)
            else -> 0
        }
    }

    LaunchedEffect(selectedGameId, selectedIndex, itemCount) {
        if (itemCount <= 0) return@LaunchedEffect
        runCatching { listState.animateScrollToItem(selectedIndex) }
    }

    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(contentPadding),
    ) {
        header()
        importStatus()

        Spacer(modifier = Modifier.weight(0.18f))

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(6.dp),
        ) {
            if (isImportSelected) {
                Text(
                    text = if (isImporting) "Importing…" else "Import Game",
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    color = BachataPalette.Primary,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                Text(
                    text = if (isImporting) "Please wait while the package installs" else "Add a PS4 game folder or PKG",
                    style = MaterialTheme.typography.titleMedium,
                    color = BachataPalette.Secondary,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            } else if (selected != null) {
                Text(
                    text = selected.title,
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                    color = BachataPalette.Primary,
                    textAlign = TextAlign.Center,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                val subtitle = selected.subtitle
                if (!subtitle.isNullOrBlank()) {
                    Text(
                        text = subtitle,
                        style = MaterialTheme.typography.titleMedium,
                        color = BachataPalette.Accent,
                        textAlign = TextAlign.Center,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                } else {
                    Text(
                        text = "Press A to launch · X for details",
                        style = MaterialTheme.typography.bodyMedium,
                        color = BachataPalette.Secondary,
                        textAlign = TextAlign.Center,
                    )
                }
            } else {
                Text(
                    text = "Your library is empty",
                    style = MaterialTheme.typography.headlineSmall,
                    fontWeight = FontWeight.SemiBold,
                    color = BachataPalette.Primary,
                )
                Text(
                    text = "Import a game to get started",
                    style = MaterialTheme.typography.bodyMedium,
                    color = BachataPalette.Secondary,
                )
            }
        }

        Spacer(modifier = Modifier.weight(0.12f))

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .weight(0.70f),
            contentAlignment = Alignment.Center,
        ) {
            // Soft edge fade so the row feels infinite / console-like.
            LazyRow(
                state = listState,
                flingBehavior = flingBehavior,
                contentPadding = PaddingValues(horizontal = 120.dp),
                horizontalArrangement = Arrangement.spacedBy(20.dp),
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxSize()
                    .padding(vertical = 8.dp),
            ) {
                itemsIndexed(games, key = { _, game -> game.id }) { index, game ->
                    val focused = game.id == selectedGameId
                    CarouselCoverItem(
                        focused = focused,
                        onClick = {
                            onSelectGame(game.id)
                            onShowDetails(game.id)
                        },
                    ) {
                        GameCover(
                            relativePath = game.relativePath,
                            modifier = Modifier.fillMaxSize(),
                        )
                    }
                }
                item(key = "import_card") {
                    val focused = isImportSelected
                    CarouselCoverItem(
                        focused = focused,
                        onClick = {
                            if (isImporting) return@CarouselCoverItem
                            onSelectGame("__import_card__")
                            onImport()
                        },
                    ) {
                        Box(
                            modifier = Modifier
                                .fillMaxSize()
                                .background(BachataPalette.Canvas.copy(alpha = 0.45f)),
                            contentAlignment = Alignment.Center,
                        ) {
                            Text(
                                text = if (isImporting) "…" else "+",
                                style = MaterialTheme.typography.displayLarge,
                                color = if (isImporting) BachataPalette.Secondary else BachataPalette.Accent,
                                fontWeight = FontWeight.Bold,
                            )
                        }
                    }
                }
            }

            Box(
                modifier = Modifier
                    .align(Alignment.CenterStart)
                    .fillMaxHeight()
                    .width(72.dp)
                    .background(
                        Brush.horizontalGradient(
                            colors = listOf(BachataPalette.Canvas, Color.Transparent),
                        ),
                    ),
            )
            Box(
                modifier = Modifier
                    .align(Alignment.CenterEnd)
                    .fillMaxHeight()
                    .width(72.dp)
                    .background(
                        Brush.horizontalGradient(
                            colors = listOf(Color.Transparent, BachataPalette.Canvas),
                        ),
                    ),
            )
        }

        Spacer(modifier = Modifier.height(8.dp))
    }
}

@Composable
private fun CarouselCoverItem(
    focused: Boolean,
    onClick: () -> Unit,
    content: @Composable () -> Unit,
) {
    val scale by animateFloatAsState(
        targetValue = if (focused) 1.08f else 0.88f,
        animationSpec = tween(durationMillis = 220),
        label = "carouselScale",
    )
    val alpha by animateFloatAsState(
        targetValue = if (focused) 1f else 0.55f,
        animationSpec = tween(durationMillis = 220),
        label = "carouselAlpha",
    )
    val width = if (focused) 176.dp else 136.dp
    Box(
        modifier = Modifier
            .width(width)
            .aspectRatio(0.72f)
            .scale(scale)
            .graphicsLayer { this.alpha = alpha }
            .clip(RoundedCornerShape(16.dp))
            .border(
                width = if (focused) 2.5.dp else 1.dp,
                color = if (focused) BachataPalette.Accent else Color.White.copy(alpha = 0.14f),
                shape = RoundedCornerShape(16.dp),
            )
            .clickable(onClick = onClick),
        contentAlignment = Alignment.Center,
    ) {
        content()
    }
}
