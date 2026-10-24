@file:OptIn(ExperimentalMaterial3Api::class)

package com.jpo.cellnet.ui

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.CallEnd
import androidx.compose.material.icons.filled.CellTower
import androidx.compose.material.icons.filled.Chat
import androidx.compose.material.icons.filled.Mic
import androidx.compose.material.icons.filled.MicOff
import androidx.compose.material.icons.filled.People
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Send
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.VolumeOff
import androidx.compose.material.icons.filled.VolumeUp
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledIconButton
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import com.jpo.cellnet.call.CallPhase
import com.jpo.cellnet.data.CallEntity
import com.jpo.cellnet.data.CallRecordState
import com.jpo.cellnet.data.ConversationEntity
import com.jpo.cellnet.data.MessageEntity
import com.jpo.cellnet.data.MessageState
import com.jpo.cellnet.net.Cells
import com.jpo.cellnet.net.DirectoryUser
import com.jpo.cellnet.wifi.SsidStatus
import kotlinx.coroutines.launch
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

private val timeFormat = SimpleDateFormat("HH:mm", Locale.getDefault())

private fun formatTime(millis: Long): String =
    if (millis <= 0L) "" else timeFormat.format(Date(millis))

private fun formatDuration(millis: Long): String {
    val totalSeconds = (millis / 1000L).coerceAtLeast(0L)
    return "%d:%02d".format(totalSeconds / 60L, totalSeconds % 60L)
}

@Composable
fun CellNetRoot(
    state: UiState,
    conversations: List<ConversationEntity>,
    messages: List<MessageEntity>,
    callHistory: List<CallEntity>,
    viewModel: AppViewModel
) {
    val context = LocalContext.current
    val snackbarHostState = remember { SnackbarHostState() }
    val scope = rememberCoroutineScope()

    val recordPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> if (granted) viewModel.startRecording() }

    // A live call needs the microphone as well. The permission is requested at
    // the moment the user actually calls, and the call is placed on the answer.
    var pendingCall by remember { mutableStateOf<Pair<String, String>?>(null) }
    val callPermission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        val target = pendingCall
        pendingCall = null
        if (granted && target != null) {
            viewModel.startCall(target.first, target.second)
        } else if (!granted) {
            scope.launch {
                snackbarHostState.showSnackbar(
                    "Microphone refusé : un appel en direct est impossible"
                )
            }
        }
    }

    val placeCall: (String, String) -> Unit = { peerId, peerName ->
        val granted = ContextCompat.checkSelfPermission(
            context, Manifest.permission.RECORD_AUDIO
        ) == PackageManager.PERMISSION_GRANTED
        if (granted) {
            viewModel.startCall(peerId, peerName)
        } else {
            pendingCall = peerId to peerName
            callPermission.launch(Manifest.permission.RECORD_AUDIO)
        }
    }

    val wifiPermissions = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { result -> viewModel.onPermissionsResult(result) }

    val requiredWifiPermissions = remember {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            arrayOf(
                Manifest.permission.NEARBY_WIFI_DEVICES,
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }
    }

    val requestWifiPermissions = {
        val missing = requiredWifiPermissions.filter {
            ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) {
            viewModel.refreshCell()
        } else {
            wifiPermissions.launch(missing.toTypedArray())
        }
    }

    LaunchedEffect(Unit) {
        // Requested once at start; the Cell tab offers the same action again.
        requestWifiPermissions()
    }

    val notice = state.error ?: state.info
    LaunchedEffect(notice) {
        if (notice != null) {
            snackbarHostState.showSnackbar(notice)
            viewModel.clearMessages()
        }
    }

    Scaffold(snackbarHost = { SnackbarHost(snackbarHostState) }) { padding ->
        Box(Modifier.padding(padding)) {
            when {
                !state.ready -> LoadingScreen()
                state.profile == null -> SetupScreen(onSubmit = viewModel::saveProfile)
                state.activePeer != null -> ChatScreen(
                    state = state,
                    messages = messages,
                    lastCall = callHistory.firstOrNull { it.peerId == state.activePeer },
                    onBack = viewModel::closeConversation,
                    onSend = viewModel::sendText,
                    onCall = { placeCall(state.activePeer.orEmpty(), state.activePeerName.orEmpty()) },
                    onStartRecording = {
                        val granted = ContextCompat.checkSelfPermission(
                            context, Manifest.permission.RECORD_AUDIO
                        ) == PackageManager.PERMISSION_GRANTED
                        if (granted) viewModel.startRecording()
                        else recordPermission.launch(Manifest.permission.RECORD_AUDIO)
                    },
                    onStopRecording = viewModel::stopRecordingAndSend,
                    onCancelRecording = viewModel::cancelRecording,
                    onPlayVoice = viewModel::playVoice
                )
                else -> HomeScreen(
                    state = state,
                    conversations = conversations,
                    viewModel = viewModel,
                    onRequestWifiPermissions = requestWifiPermissions
                )
            }

            // The call screen covers the application while a call is up: it is the
            // only way to reach the hang up / mute / speaker controls.
            CallOverlay(
                call = state.call,
                onAccept = viewModel::acceptCall,
                onReject = viewModel::rejectCall,
                onHangUp = viewModel::hangupCall,
                onToggleMute = viewModel::toggleCallMute,
                onToggleSpeaker = viewModel::toggleCallSpeaker
            )
        }
    }
}

@Composable
private fun LoadingScreen() {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        CircularProgressIndicator()
    }
}

@Composable
private fun SetupScreen(onSubmit: (String) -> Unit) {
    var name by remember { mutableStateOf("") }
    Column(
        Modifier
            .fillMaxSize()
            .padding(24.dp),
        verticalArrangement = Arrangement.Center
    ) {
        Icon(
            Icons.Default.CellTower,
            contentDescription = null,
            modifier = Modifier.size(56.dp),
            tint = MaterialTheme.colorScheme.primary
        )
        Spacer(Modifier.height(16.dp))
        Text(
            "Bienvenue sur CellNet",
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Bold
        )
        Spacer(Modifier.height(8.dp))
        Text(
            "Réseau local à trois cellules ESP32, sans Internet ni serveur central.",
            style = MaterialTheme.typography.bodyMedium
        )
        Spacer(Modifier.height(24.dp))
        OutlinedTextField(
            value = name,
            onValueChange = { name = it },
            label = { Text("Votre nom d'affichage") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(16.dp))
        Button(
            onClick = { if (name.isNotBlank()) onSubmit(name) },
            enabled = name.isNotBlank(),
            modifier = Modifier.fillMaxWidth()
        ) {
            Text("Créer mon identité")
        }
        Spacer(Modifier.height(16.dp))
        Text(
            "Un identifiant stable est généré sur ce téléphone et réutilisé à chaque " +
                "démarrage. Le nom d'affichage n'est pas une preuve d'identité : ce " +
                "prototype ne fournit pas d'authentification.",
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun EmptyState(icon: ImageVector, title: String, subtitle: String) {
    Column(
        Modifier
            .fillMaxSize()
            .padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally
    ) {
        Icon(icon, contentDescription = null, modifier = Modifier.size(48.dp))
        Spacer(Modifier.height(12.dp))
        Text(title, style = MaterialTheme.typography.titleMedium)
        Spacer(Modifier.height(4.dp))
        Text(
            subtitle,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
    }
}

@Composable
private fun Avatar(name: String) {
    val initial = name.trim().take(1).uppercase(Locale.getDefault()).ifBlank { "?" }
    Box(
        Modifier
            .size(40.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.primaryContainer),
        contentAlignment = Alignment.Center
    ) {
        Text(
            initial,
            fontWeight = FontWeight.Bold,
            color = MaterialTheme.colorScheme.onPrimaryContainer
        )
    }
}

@Composable
private fun StationChip(station: String) {
    Surface(
        color = MaterialTheme.colorScheme.secondary.copy(alpha = 0.15f),
        shape = RoundedCornerShape(8.dp)
    ) {
        Text(
            station,
            style = MaterialTheme.typography.labelSmall,
            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp)
        )
    }
}

@Composable
private fun BadgeCount(count: Int) {
    Box(
        Modifier
            .size(20.dp)
            .clip(CircleShape)
            .background(MaterialTheme.colorScheme.primary),
        contentAlignment = Alignment.Center
    ) {
        Text(
            count.toString(),
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onPrimary
        )
    }
}

@Composable
private fun InfoCard(title: String, rows: List<Pair<String, String>>) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(14.dp)) {
            Text(
                title,
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.SemiBold
            )
            Spacer(Modifier.height(8.dp))
            rows.forEachIndexed { index, row ->
                if (index > 0) HorizontalDivider(Modifier.padding(vertical = 6.dp))
                Row(Modifier.fillMaxWidth()) {
                    Text(
                        row.first,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(120.dp)
                    )
                    Text(
                        row.second,
                        style = MaterialTheme.typography.bodySmall,
                        fontWeight = FontWeight.Medium,
                        modifier = Modifier.weight(1f)
                    )
                }
            }
        }
    }
}

@Composable
private fun ConnectionBanner(state: UiState, onRequestPermissions: () -> Unit) {
    val container = when (state.cellStatus) {
        CellStatus.CONNECTED -> MaterialTheme.colorScheme.primaryContainer
        CellStatus.UNKNOWN -> MaterialTheme.colorScheme.surfaceVariant
        else -> MaterialTheme.colorScheme.errorContainer
    }
    val text = when (state.cellStatus) {
        CellStatus.CONNECTED ->
            if (state.wsConnected) {
                "Connecté à ${state.cellSite} · ${state.wifiSsid ?: "SSID indisponible"}"
            } else {
                "Station ${state.cellSite} jointe, temps réel en reconnexion…"
            }
        CellStatus.NO_WIFI -> "Aucun Wi-Fi connecté"
        CellStatus.PERMISSION_MISSING -> "Permissions Wi-Fi manquantes : identification impossible"
        CellStatus.SSID_UNAVAILABLE -> "Wi-Fi connecté — identification de la cellule indisponible"
        CellStatus.SSID_UNKNOWN ->
            "Wi-Fi « ${state.wifiSsid ?: "?"} » : ce n'est pas une cellule CellNet"
        CellStatus.STATION_UNREACHABLE -> "Cellule ${state.cellSite} détectée — station inaccessible"
        CellStatus.STATION_MISMATCH ->
            "Cellule ${state.cellSite} : la station annonce ${state.stationSite}"
        CellStatus.UNKNOWN -> "Vérification de la cellule…"
    }

    Surface(color = container, modifier = Modifier.fillMaxWidth()) {
        Row(
            Modifier.padding(horizontal = 16.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                if (state.wifiConnected) Icons.Default.Wifi else Icons.Default.Refresh,
                contentDescription = null,
                modifier = Modifier.size(16.dp)
            )
            Spacer(Modifier.width(8.dp))
            Text(
                text,
                style = MaterialTheme.typography.labelMedium,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f)
            )
            if (state.cellStatus == CellStatus.PERMISSION_MISSING) {
                TextButton(onClick = onRequestPermissions) { Text("Autoriser") }
            }
        }
    }
}

@Composable
private fun HomeScreen(
    state: UiState,
    conversations: List<ConversationEntity>,
    viewModel: AppViewModel,
    onRequestWifiPermissions: () -> Unit
) {
    var tab by remember { mutableIntStateOf(0) }
    Scaffold(
        topBar = {
            TopAppBar(
                title = {
                    Column {
                        Text(
                            "CellNet",
                            style = MaterialTheme.typography.titleLarge,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            when {
                                state.cellSite != null && state.wifiSsid != null ->
                                    "${state.cellSite} · ${state.wifiSsid}"
                                state.cellSite != null -> state.cellSite.orEmpty()
                                state.wifiConnected ->
                                    "Wi-Fi : ${state.wifiSsid ?: "SSID indisponible"}"
                                else -> "Hors cellule"
                            },
                            style = MaterialTheme.typography.labelMedium
                        )
                    }
                },
                actions = {
                    IconButton(onClick = viewModel::refresh) {
                        Icon(Icons.Default.Refresh, contentDescription = "Rafraîchir")
                    }
                }
            )
        },
        bottomBar = {
            NavigationBar {
                NavigationBarItem(
                    selected = tab == 0,
                    onClick = { tab = 0 },
                    icon = { Icon(Icons.Default.Chat, contentDescription = null) },
                    label = { Text("Messages") }
                )
                NavigationBarItem(
                    selected = tab == 1,
                    onClick = { tab = 1 },
                    icon = { Icon(Icons.Default.People, contentDescription = null) },
                    label = { Text("Répertoire") }
                )
                NavigationBarItem(
                    selected = tab == 2,
                    onClick = { tab = 2 },
                    icon = { Icon(Icons.Default.CellTower, contentDescription = null) },
                    label = { Text("Cellule") }
                )
            }
        }
    ) { padding ->
        Column(
            Modifier
                .padding(padding)
                .fillMaxSize()
        ) {
            ConnectionBanner(state, onRequestWifiPermissions)
            when (tab) {
                0 -> ConversationsScreen(state, conversations) { peerId, name ->
                    viewModel.openConversation(peerId, name)
                }
                1 -> DirectoryScreen(state) { user ->
                    viewModel.openConversation(user.id, user.name)
                }
                else -> CellScreen(state, viewModel, onRequestWifiPermissions)
            }
        }
    }
}

@Composable
private fun ConversationsScreen(
    state: UiState,
    conversations: List<ConversationEntity>,
    onOpen: (String, String) -> Unit
) {
    if (conversations.isEmpty()) {
        EmptyState(
            Icons.Default.Chat,
            "Aucune conversation",
            "Ouvrez le répertoire pour choisir un destinataire, puis envoyez un message " +
                "texte ou vocal."
        )
        return
    }
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        items(conversations, key = { it.peerId }) { conversation ->
            Card(
                onClick = { onOpen(conversation.peerId, conversation.peerName) },
                modifier = Modifier.fillMaxWidth()
            ) {
                Row(
                    Modifier.padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Avatar(conversation.peerName)
                    Spacer(Modifier.width(12.dp))
                    Column(Modifier.weight(1f)) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text(
                                conversation.peerName,
                                fontWeight = FontWeight.SemiBold,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                            conversation.station?.let {
                                Spacer(Modifier.width(6.dp))
                                StationChip(it)
                            }
                        }
                        Text(
                            conversation.lastPreview.ifBlank { "—" },
                            style = MaterialTheme.typography.bodySmall,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    }
                    Spacer(Modifier.width(8.dp))
                    Column(horizontalAlignment = Alignment.End) {
                        Text(
                            formatTime(conversation.lastMessageAt),
                            style = MaterialTheme.typography.labelSmall
                        )
                        if (conversation.unread > 0) {
                            Spacer(Modifier.height(4.dp))
                            BadgeCount(conversation.unread)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun DirectoryScreen(state: UiState, onOpen: (DirectoryUser) -> Unit) {
    var query by remember { mutableStateOf("") }
    val filtered = remember(state.users, query) {
        val clean = query.trim()
        if (clean.isEmpty()) {
            state.users
        } else {
            state.users.filter {
                it.name.contains(clean, ignoreCase = true) ||
                    it.id.contains(clean, ignoreCase = true)
            }
        }
    }

    Column(
        Modifier
            .fillMaxSize()
            .padding(horizontal = 12.dp)
    ) {
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier
                .fillMaxWidth()
                .padding(vertical = 8.dp),
            placeholder = { Text("Rechercher un nom ou un identifiant") },
            leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
            singleLine = true
        )
        Text(
            "${filtered.size} utilisateur(s) découvert(s) via le bus RS485",
            style = MaterialTheme.typography.labelSmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(Modifier.height(8.dp))
        if (filtered.isEmpty()) {
            EmptyState(
                Icons.Default.People,
                "Aucun utilisateur",
                "Les autres téléphones apparaissent dès qu'ils se connectent à leur " +
                    "cellule et publient leur présence."
            )
        } else {
            LazyColumn(
                verticalArrangement = Arrangement.spacedBy(8.dp),
                contentPadding = PaddingValues(bottom = 16.dp)
            ) {
                items(filtered, key = { it.id }) { user ->
                    Card(onClick = { onOpen(user) }, modifier = Modifier.fillMaxWidth()) {
                        Row(
                            Modifier.padding(12.dp),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Avatar(user.name)
                            Spacer(Modifier.width(12.dp))
                            Column(Modifier.weight(1f)) {
                                Row(verticalAlignment = Alignment.CenterVertically) {
                                    Text(user.name, fontWeight = FontWeight.SemiBold)
                                    Spacer(Modifier.width(6.dp))
                                    StationChip(user.station.ifBlank { "?" })
                                }
                                Text(
                                    user.id,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            Text(
                                if (user.online) "en ligne" else "hors ligne",
                                style = MaterialTheme.typography.labelSmall,
                                color = if (user.online) MaterialTheme.colorScheme.secondary
                                else MaterialTheme.colorScheme.onSurfaceVariant
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun CellScreen(
    state: UiState,
    viewModel: AppViewModel,
    onRequestPermissions: () -> Unit
) {
    LazyColumn(
        Modifier.fillMaxSize(),
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item {
            InfoCard(
                "Wi-Fi du téléphone",
                listOf(
                    "Wi-Fi" to if (state.wifiConnected) "connecté" else "non connecté",
                    "SSID actuel" to (state.wifiSsid ?: ssidUnavailableLabel(state)),
                    "Passerelle" to (state.wifiGateway ?: "—"),
                    "Localisation" to if (state.locationServicesEnabled) "activée" else "désactivée",
                    "Permissions" to permissionLabel(state)
                )
            )
        }
        item {
            InfoCard(
                "Cellule et station",
                listOf(
                    "Cellule" to cellLabel(state),
                    "État" to cellStatusLabel(state),
                    "Station" to stationLabel(state),
                    "Temps réel" to if (state.wsConnected) "connecté" else "hors ligne",
                    "Utilisateur" to (state.profile?.displayName ?: "—"),
                    "Identifiant" to (state.profile?.userId ?: "—"),
                    "Dernière coupure" to (state.lastDisconnect ?: "—")
                )
            )
        }
        if (state.cellStatusDetail != null) {
            item {
                Text(
                    state.cellStatusDetail,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
        if (state.missingPermissions.isNotEmpty()) {
            item {
                Button(onClick = onRequestPermissions, modifier = Modifier.fillMaxWidth()) {
                    Text("Autoriser l'accès Wi-Fi")
                }
            }
        }
        item {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = viewModel::scanNearbyCells,
                    enabled = !state.scanning,
                    modifier = Modifier.weight(1f)
                ) {
                    Icon(Icons.Default.Wifi, contentDescription = null)
                    Spacer(Modifier.width(6.dp))
                    Text(if (state.scanning) "Scan…" else "Scanner")
                }
                OutlinedButton(
                    onClick = viewModel::refreshCell,
                    modifier = Modifier.weight(1f)
                ) {
                    Text("Revérifier")
                }
            }
        }
        item {
            Text("Cellules", style = MaterialTheme.typography.titleMedium)
        }
        items(Cells.all, key = { it.ssid }) { cell ->
            val rssi = state.nearCells.firstOrNull { it.first == cell.ssid }?.second
            val current = state.wifiSsid.equals(cell.ssid, ignoreCase = true)
            Card(Modifier.fillMaxWidth()) {
                Row(
                    Modifier.padding(12.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(Icons.Default.Wifi, contentDescription = null)
                    Spacer(Modifier.width(12.dp))
                    Column(Modifier.weight(1f)) {
                        Text(cell.site, fontWeight = FontWeight.SemiBold)
                        Text(
                            cell.ssid + (rssi?.let { " · $it dBm" } ?: ""),
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                    if (current) {
                        Text(
                            "connecté",
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.secondary
                        )
                    } else {
                        TextButton(onClick = { viewModel.requestCell(cell.site) }) {
                            Text("Rejoindre")
                        }
                    }
                }
            }
        }
        if (state.wifiMessage != null) {
            item {
                Text(
                    "Wi-Fi : ${state.wifiMessage}",
                    style = MaterialTheme.typography.labelSmall
                )
            }
        }
        item {
            Text(
                "Android ne laisse pas une application changer de réseau Wi-Fi en " +
                    "silence : le système affiche sa propre demande de confirmation. " +
                    "Si le changement est refusé, connectez le téléphone au SSID de la " +
                    "cellule depuis les réglages puis revenez dans l'application : la " +
                    "cellule est redétectée automatiquement.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun ChatScreen(
    state: UiState,
    messages: List<MessageEntity>,
    lastCall: CallEntity?,
    onBack: () -> Unit,
    onSend: (String) -> Unit,
    onCall: () -> Unit,
    onStartRecording: () -> Unit,
    onStopRecording: () -> Unit,
    onCancelRecording: () -> Unit,
    onPlayVoice: (MessageEntity) -> Unit
) {
    var draft by remember { mutableStateOf("") }
    val listState = rememberLazyListState()

    LaunchedEffect(messages.size) {
        if (messages.isNotEmpty()) {
            listState.animateScrollToItem(messages.lastIndex)
        }
    }

    // A live call is possible only between two users of the same cell: the
    // button is disabled (with the reason in the subtitle) otherwise.
    val callAllowed = state.cellSite != null && state.httpConnected && state.wsConnected

    Scaffold(
        topBar = {
            TopAppBar(
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Retour")
                    }
                },
                title = {
                    Column {
                        Text(
                            state.activePeerName ?: "Conversation",
                            fontWeight = FontWeight.SemiBold,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Text(
                            state.cellSite?.let { "Vous êtes sur $it" } ?: "Hors cellule",
                            style = MaterialTheme.typography.labelSmall
                        )
                    }
                },
                actions = {
                    IconButton(onClick = onCall, enabled = callAllowed) {
                        Icon(
                            Icons.Default.Call,
                            contentDescription = if (callAllowed) {
                                "Appeler (même cellule)"
                            } else {
                                "Appel indisponible hors cellule"
                            }
                        )
                    }
                }
            )
        },
        bottomBar = {
            Column {
                // Journal of the last live call with this contact. Informative
                // only: it never replaces the messages.
                if (lastCall != null) {
                    CallHistoryRow(lastCall)
                }
                Composer(
                    state = state,
                    draft = draft,
                    onDraft = { draft = it },
                    onSend = {
                        if (draft.isNotBlank()) {
                            onSend(draft)
                            draft = ""
                        }
                    },
                    onStartRecording = onStartRecording,
                    onStopRecording = onStopRecording,
                    onCancelRecording = onCancelRecording
                )
            }
        }
    ) { padding ->
        if (messages.isEmpty()) {
            Box(
                Modifier
                    .padding(padding)
                    .fillMaxSize(),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    "Aucun message. Écrivez ou enregistrez un vocal (60 s maximum).",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(24.dp)
                )
            }
        } else {
            LazyColumn(
                state = listState,
                modifier = Modifier
                    .padding(padding)
                    .fillMaxSize(),
                contentPadding = PaddingValues(12.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                items(messages, key = { it.id }) { message ->
                    MessageBubble(
                        message = message,
                        playing = state.playingMessage == message.id,
                        sending = state.sendingMessage == message.id,
                        sendProgress = state.sendProgress,
                        onPlay = { onPlayVoice(message) }
                    )
                }
            }
        }
    }
}

@Composable
private fun Composer(
    state: UiState,
    draft: String,
    onDraft: (String) -> Unit,
    onSend: () -> Unit,
    onStartRecording: () -> Unit,
    onStopRecording: () -> Unit,
    onCancelRecording: () -> Unit
) {
    Surface(tonalElevation = 3.dp) {
        Column(
            Modifier
                .fillMaxWidth()
                .padding(8.dp)
        ) {
            if (state.recording) {
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Box(
                        Modifier
                            .size(10.dp)
                            .clip(CircleShape)
                            .background(Color(0xFFD32F2F))
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        "${formatDuration(state.recordingMs)} / " +
                            "${state.maxRecordingSeconds} s — arrêt automatique",
                        modifier = Modifier.weight(1f),
                        style = MaterialTheme.typography.bodySmall
                    )
                    TextButton(onClick = onCancelRecording) { Text("Annuler") }
                    Button(onClick = onStopRecording) {
                        Icon(Icons.Default.Send, contentDescription = null)
                        Spacer(Modifier.width(4.dp))
                        Text("Envoyer")
                    }
                }
            } else {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    OutlinedTextField(
                        value = draft,
                        onValueChange = onDraft,
                        modifier = Modifier.weight(1f),
                        placeholder = { Text("Votre message") },
                        maxLines = 4
                    )
                    IconButton(onClick = onStartRecording) {
                        Icon(Icons.Default.Mic, contentDescription = "Enregistrer un vocal")
                    }
                    FilledIconButton(onClick = onSend, enabled = draft.isNotBlank()) {
                        Icon(Icons.Default.Send, contentDescription = "Envoyer")
                    }
                }
                if (state.sendingMessage != null) {
                    Spacer(Modifier.height(6.dp))
                    LinearProgressIndicator(
                        progress = { state.sendProgress },
                        modifier = Modifier.fillMaxWidth()
                    )
                    Text(
                        "Envoi du vocal : ${(state.sendProgress * 100).toInt()} %",
                        style = MaterialTheme.typography.labelSmall
                    )
                }
            }
        }
    }
}

@Composable
private fun MessageBubble(
    message: MessageEntity,
    playing: Boolean,
    sending: Boolean,
    sendProgress: Float,
    onPlay: () -> Unit
) {
    val mine = message.isMine
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = if (mine) Arrangement.End else Arrangement.Start
    ) {
        Surface(
            color = if (mine) MaterialTheme.colorScheme.primaryContainer
            else MaterialTheme.colorScheme.surfaceVariant,
            shape = RoundedCornerShape(16.dp),
            modifier = Modifier.widthIn(max = 320.dp)
        ) {
            Column(Modifier.padding(10.dp)) {
                if (message.kind == "voice") {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        IconButton(onClick = onPlay, enabled = message.voicePath != null) {
                            Icon(
                                if (playing) Icons.Default.Stop else Icons.Default.PlayArrow,
                                contentDescription = "Lire"
                            )
                        }
                        Column {
                            Text("Message vocal", fontWeight = FontWeight.SemiBold)
                            Text(
                                formatDuration(message.voiceDurationMs),
                                style = MaterialTheme.typography.bodySmall
                            )
                        }
                    }
                    if (sending) {
                        LinearProgressIndicator(
                            progress = { sendProgress },
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 4.dp)
                        )
                    } else if (!mine && message.state == MessageState.RECEIVING) {
                        val total = message.voiceTotalBlocks.coerceAtLeast(1)
                        LinearProgressIndicator(
                            progress = { message.voiceReceivedBlocks.toFloat() / total },
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 4.dp)
                        )
                    }
                } else {
                    Text(message.body)
                }
                Row(
                    Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp),
                    horizontalArrangement = Arrangement.End,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        formatTime(message.createdAt),
                        style = MaterialTheme.typography.labelSmall
                    )
                    val label = stateLabel(message)
                    if (label != null) {
                        Spacer(Modifier.width(6.dp))
                        Text(label, style = MaterialTheme.typography.labelSmall)
                    }
                }
            }
        }
    }
}

private fun stateLabel(message: MessageEntity): String? = when {
    message.error != null -> "Échec"
    message.state == MessageState.PENDING -> "En attente"
    message.state == MessageState.SENDING -> "Envoi…"
    message.state == MessageState.SENT -> if (message.isMine) "Transmis" else "Reçu"
    message.state == MessageState.DELIVERED -> "Livré"
    message.state == MessageState.READ -> if (message.kind == "voice") "Écouté" else "Lu"
    message.state == MessageState.RECEIVING ->
        "Réception ${message.voiceReceivedBlocks}/${message.voiceTotalBlocks}"
    message.state == MessageState.RECEIVED -> "Reçu"
    else -> null
}

// ---------------------------------------------------------------------------
// Labels of the Wi-Fi detection states (never invent a cell name)
// ---------------------------------------------------------------------------
private fun ssidUnavailableLabel(state: UiState): String = when (state.ssidStatus) {
    SsidStatus.AVAILABLE -> "—"
    SsidStatus.NO_PERMISSION -> "indisponible (permission refusée)"
    SsidStatus.REDACTED -> "indisponible (masqué par Android)"
    SsidStatus.EMPTY -> "indisponible (aucune valeur)"
    SsidStatus.ERROR -> "indisponible (erreur de lecture)"
}

private fun permissionLabel(state: UiState): String =
    if (state.missingPermissions.isEmpty()) {
        "accordées"
    } else {
        "manquantes : " + state.missingPermissions.joinToString {
            it.substringAfterLast('.')
        }
    }

private fun cellLabel(state: UiState): String {
    val site = state.cellSite ?: return "non identifiée"
    val via = state.cellIdentifiedBy
    return if (via != null) "$site (via $via)" else site
}

private fun cellStatusLabel(state: UiState): String = when (state.cellStatus) {
    CellStatus.CONNECTED -> "connectée"
    CellStatus.NO_WIFI -> "aucun Wi-Fi"
    CellStatus.PERMISSION_MISSING -> "permissions insuffisantes"
    CellStatus.SSID_UNAVAILABLE -> "SSID illisible"
    CellStatus.SSID_UNKNOWN -> "réseau inconnu"
    CellStatus.STATION_UNREACHABLE -> "station inaccessible"
    CellStatus.STATION_MISMATCH -> "station incohérente"
    CellStatus.UNKNOWN -> "vérification…"
}

private fun stationLabel(state: UiState): String = when {
    state.httpConnected -> "joignable sur ${state.cellIp}"
    state.cellSite != null -> "inaccessible (${state.cellIp ?: "adresse inconnue"})"
    else -> "—"
}

// ---------------------------------------------------------------------------
// Live calls
// ---------------------------------------------------------------------------

/** One line summarising the last live call with a contact. */
@Composable
private fun CallHistoryRow(call: CallEntity) {
    val label = when (call.state) {
        CallRecordState.ACTIVE -> "Appel en cours"
        CallRecordState.MISSED -> "Appel manqué"
        CallRecordState.REFUSED -> "Appel refusé"
        CallRecordState.FAILED -> "Appel échoué"
        CallRecordState.DIALING, CallRecordState.RINGING -> "Appel en cours…"
        else -> if (call.direction == "out") "Appel sortant" else "Appel entrant"
    }
    val duration = if (call.durationMs > 0L) " · ${formatDuration(call.durationMs)}" else ""
    Surface(tonalElevation = 1.dp) {
        Row(
            Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Icon(
                Icons.Default.Call,
                contentDescription = null,
                modifier = Modifier.size(16.dp),
                tint = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.width(8.dp))
            Text(
                "$label$duration · ${formatTime(call.startedAt)}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

/** Status line shown on the call screen. */
private fun callStatusLabel(call: CallUiState): String = when (call.phase) {
    CallPhase.DIALING -> "Appel en cours…"
    CallPhase.RINGING -> "Appel entrant"
    CallPhase.ACTIVE -> "En communication"
    CallPhase.ENDED, CallPhase.IDLE -> call.detail ?: ""
}



/**
 * Full screen call interface. It only appears while a call exists, so it can
 * never hide the rest of the application permanently.
 *
 * The call is local to the cell: the station relays the audio between the two
 * phones of the same access point, and nothing is sent to the RS485 bus.
 */
@Composable
private fun CallOverlay(
    call: CallUiState,
    onAccept: () -> Unit,
    onReject: () -> Unit,
    onHangUp: () -> Unit,
    onToggleMute: () -> Unit,
    onToggleSpeaker: () -> Unit
) {
    if (!call.busy) return
    val incomingRinging = call.incoming && call.phase == CallPhase.RINGING

    Surface(
        color = MaterialTheme.colorScheme.surface,
        modifier = Modifier.fillMaxSize()
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Avatar(call.peerName ?: call.peerId ?: "?")
            Spacer(Modifier.height(16.dp))
            Text(
                call.peerName ?: call.peerId ?: "Appel",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Spacer(Modifier.height(6.dp))
            Text(
                callStatusLabel(call),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Text(
                "Appel local à la cellule uniquement",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            if (call.phase == CallPhase.ACTIVE) {
                Spacer(Modifier.height(10.dp))
                Text(
                    formatDuration(call.durationMs),
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.SemiBold
                )
                Text(
                    if (call.speaker) "Haut-parleur" else "Écouteur",
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }

            Spacer(Modifier.height(32.dp))

            if (incomingRinging) {
                Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                    Button(onClick = onAccept) {
                        Icon(Icons.Default.Call, contentDescription = null)
                        Spacer(Modifier.width(6.dp))
                        Text("Accepter")
                    }
                    OutlinedButton(onClick = onReject) {
                        Icon(Icons.Default.CallEnd, contentDescription = null)
                        Spacer(Modifier.width(6.dp))
                        Text("Refuser")
                    }
                }
            } else {
                Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                    IconButton(onClick = onToggleMute) {
                        Icon(
                            if (call.muted) Icons.Default.MicOff else Icons.Default.Mic,
                            contentDescription =
                                if (call.muted) "Réactiver le micro" else "Couper le micro"
                        )
                    }
                    IconButton(onClick = onToggleSpeaker) {
                        Icon(
                            if (call.speaker) Icons.Default.VolumeUp else Icons.Default.VolumeOff,
                            contentDescription =
                                if (call.speaker) "Passer à l'écouteur" else "Passer au haut-parleur"
                        )
                    }
                }
                Spacer(Modifier.height(20.dp))
                Button(
                    onClick = onHangUp,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color(0xFFD32F2F),
                        contentColor = Color.White
                    )
                ) {
                    Icon(Icons.Default.CallEnd, contentDescription = null)
                    Spacer(Modifier.width(6.dp))
                    Text(if (call.phase == CallPhase.DIALING) "Annuler" else "Raccrocher")
                }
            }
        }
    }
}
