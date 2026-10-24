package com.jpo.cellnet.ui

import android.app.Application
import android.util.Base64
import android.util.Log
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.jpo.cellnet.audio.VoiceAssembler
import com.jpo.cellnet.audio.VoicePlayer
import com.jpo.cellnet.audio.VoiceProtocol
import com.jpo.cellnet.audio.VoiceRecorder
import com.jpo.cellnet.data.AppDatabase
import com.jpo.cellnet.data.BlockSet
import com.jpo.cellnet.data.ConversationEntity
import com.jpo.cellnet.data.MessageEntity
import com.jpo.cellnet.data.MessageState
import com.jpo.cellnet.data.TransferEntity
import com.jpo.cellnet.data.TransferState
import com.jpo.cellnet.data.UserProfile
import com.jpo.cellnet.data.UserProfileStore
import com.jpo.cellnet.net.ApiException
import com.jpo.cellnet.net.BinaryVoiceFrame
import com.jpo.cellnet.net.CellApi
import com.jpo.cellnet.net.CellSocket
import com.jpo.cellnet.net.Cells
import com.jpo.cellnet.net.DirectoryUser
import com.jpo.cellnet.sync.OutboxPolicy
import com.jpo.cellnet.text.TextAssembler
import com.jpo.cellnet.text.TextChunker
import com.jpo.cellnet.wifi.CellWifi
import com.jpo.cellnet.wifi.SsidStatus
import com.jpo.cellnet.wifi.WifiSnapshot
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID

/**
 * Result of the cell detection, as displayed by the UI. Each value maps to a
 * distinct situation: a missing permission is never reported as "no Wi-Fi", and
 * an unreachable station is never reported as "not connected to the cell".
 */
enum class CellStatus {
    UNKNOWN,             // not checked yet
    NO_WIFI,             // no active Wi-Fi connection
    PERMISSION_MISSING,  // Wi-Fi or location permission refused
    SSID_UNAVAILABLE,    // Wi-Fi connected, SSID unreadable, no other hint
    SSID_UNKNOWN,        // SSID readable but not a configured cell
    STATION_UNREACHABLE, // cell identified, local service does not answer
    STATION_MISMATCH,    // a station answered but announces another cell
    CONNECTED            // cell identified and its service answers
}

data class UiState(
    val ready: Boolean = false,
    val profile: UserProfile? = null,
    // Phone-side Wi-Fi state
    val wifiConnected: Boolean = false,
    val wifiSsid: String? = null,
    val ssidStatus: SsidStatus = SsidStatus.ERROR,
    val wifiGateway: String? = null,
    val missingPermissions: List<String> = emptyList(),
    val locationServicesEnabled: Boolean = false,
    // Identified cell / station state
    val cellStatus: CellStatus = CellStatus.UNKNOWN,
    val cellStatusDetail: String? = null,
    val cellSite: String? = null,
    val cellIp: String? = null,
    val cellIdentifiedBy: String? = null,
    val stationSite: String? = null,
    val httpConnected: Boolean = false,
    val wsConnected: Boolean = false,
    val users: List<DirectoryUser> = emptyList(),
    val activePeer: String? = null,
    val activePeerName: String? = null,
    val recording: Boolean = false,
    val recordingMs: Long = 0L,
    val sendingMessage: String? = null,
    val sendProgress: Float = 0f,
    val playingMessage: String? = null,
    val scanning: Boolean = false,
    val nearCells: List<Pair<String, Int>> = emptyList(),
    val wifiMessage: String? = null,
    val lastDisconnect: String? = null,
    val error: String? = null,
    val info: String? = null
) {
    val connected: Boolean get() = httpConnected
    val recordingSeconds: Int get() = (recordingMs / 1000L).toInt()
    val maxRecordingSeconds: Int get() = (VoiceProtocol.MAX_DURATION_MS / 1000L).toInt()
}

@OptIn(ExperimentalCoroutinesApi::class)
class AppViewModel(application: Application) : AndroidViewModel(application) {

    private val store = UserProfileStore(application)
    private val dao = AppDatabase.get(application).chatDao()
    private val transferDao = AppDatabase.get(application).transferDao()
    private val api = CellApi()
    private val recorder = VoiceRecorder(application)
    private val player = VoicePlayer()
    private val assembler = VoiceAssembler(application)
    private val wifi = CellWifi(application)

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    private val activePeerFlow = MutableStateFlow<String?>(null)

    val conversations: StateFlow<List<ConversationEntity>> =
        dao.observeConversations().stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    val messages: StateFlow<List<MessageEntity>> = activePeerFlow.flatMapLatest { peer ->
        if (peer.isNullOrBlank()) flowOf(emptyList()) else dao.observeMessages(peer)
    }.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    private val socketListener = object : CellSocket.Listener {
        override fun onSocketOpen() {
            _state.update { it.copy(wsConnected = true, error = null) }
        }

        override fun onSocketClosed(reason: String) {
            _state.update { it.copy(wsConnected = false, lastDisconnect = reason) }
        }

        override fun onSocketText(text: String) {
            handleSocketText(text)
        }

        override fun onSocketBinary(bytes: ByteArray) {
            handleVoiceBlockData(bytes)
        }
    }

    private val socket = CellSocket(socketListener)

    private var sessionJob: Job? = null
    private var recordingJob: Job? = null
    private var monitorJob: Job? = null
    private var outboxJob: Job? = null
    private var outboxResumed = false

    private val detectionMutex = Mutex()

    /** Long text reassembly (chunks of the transfer currently in flight). */
    private val textAssembler = TextAssembler()

    /** Outbox bookkeeping: attempts and next allowed attempt per message. */
    private val outboxAttempts = HashMap<String, Int>()
    private val outboxNextAttempt = HashMap<String, Long>()

    /** Transfers to resume as soon as a station answers (missing blocks list). */
    private val pendingResumes = LinkedHashMap<String, MutableList<Int>>()

    /** Consecutive failed probes: a single failure must not tear the session down. */
    private var probeFailures = 0

    /** Station the current session is bound to, to detect a real cell change. */
    private var boundSite: String? = null

    @Volatile
    private var pendingVoiceBlock: Pair<String, Int>? = null

    init {
        wifi.startMonitoring { onNetworkChanged() }
        val profile = store.load()
        if (profile != null) {
            _state.update { it.copy(profile = profile, ready = true) }
            viewModelScope.launch(Dispatchers.IO) {
                // Interrupted transfers are resumed, never silently destroyed: the
                // partial files and the received block list are both durable.
                restoreTransfers()
                assembler.discardExpired(TRANSFER_MAX_AGE_MS, System.currentTimeMillis())
            }
            startSession()
            startOutbox()
        } else {
            _state.update { it.copy(ready = true) }
        }
    }

    // -----------------------------------------------------------------------
    // Identity
    // -----------------------------------------------------------------------
    fun saveProfile(displayName: String) {
        val profile = store.save(displayName)
        _state.update { it.copy(profile = profile, info = null, error = null) }
        startSession()
    }

    fun renameProfile(displayName: String) {
        val profile = store.rename(displayName) ?: return
        _state.update { it.copy(profile = profile) }
        viewModelScope.launch(Dispatchers.IO) {
            runCatching { api.registerPresence(profile.userId, profile.displayName) }
        }
    }

    // -----------------------------------------------------------------------
    // Connection / presence
    // -----------------------------------------------------------------------
    private fun startSession() {
        sessionJob?.cancel()
        sessionJob = viewModelScope.launch(Dispatchers.IO) {
            var tick = 0
            while (isActive) {
                try {
                    ensureConnected()
                    if (_state.value.httpConnected && tick % 6 == 0) {
                        val profile = _state.value.profile ?: return@launch
                        api.registerPresence(profile.userId, profile.displayName)
                        val users = api.users()
                        _state.update { it.copy(users = users) }
                    }
                    tick++
                    delay(5_000)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    outboxResumed = false
                    Log.e(TAG, "session loop error", error)
                    _state.update {
                        it.copy(
                            httpConnected = false,
                            wsConnected = false,
                            error = describe(error)
                        )
                    }
                    delay(5_000)
                }
            }
        }
    }

    /** One outbox attempt for a text message (single frame or chunked). */
    private suspend fun resendText(message: MessageEntity, profile: UserProfile) {
        if (TextChunker.needsFragmentation(message.body)) {
            try {
                sendTextChunks(
                    message.id, message.body, profile.userId, message.conversationId, null
                )
                outboxAttempts.remove(message.id)
                outboxNextAttempt.remove(message.id)
            } catch (error: ApiException) {
                if (!OutboxPolicy.retryable(error.status)) {
                    dao.setStateWithError(
                        message.id, MessageState.FAILED, OutboxPolicy.permanentReason(error.status)
                    )
                } else {
                    dao.setStateWithError(message.id, MessageState.FAILED, describe(error))
                }
            } catch (error: Exception) {
                dao.setStateWithError(message.id, MessageState.FAILED, describe(error))
            }
            return
        }
        val payload = JSONObject()
            .put("kind", "text")
            .put("id", message.id)
            .put("from", profile.userId)
            .put("to", message.conversationId)
            .put("ts", message.createdAt)
            .put("body", message.body)
            .toString()
        try {
            dao.setState(message.id, MessageState.SENDING)
            val response = api.sendText(payload)
            val status = response.optString("status")
            dao.setState(
                message.id,
                if (status == "delivered-local") MessageState.DELIVERED else MessageState.SENT
            )
            outboxAttempts.remove(message.id)
            outboxNextAttempt.remove(message.id)
        } catch (error: ApiException) {
            val reason = if (OutboxPolicy.retryable(error.status)) {
                describe(error)
            } else {
                OutboxPolicy.permanentReason(error.status)
            }
            dao.setStateWithError(message.id, MessageState.FAILED, reason)
        } catch (error: Exception) {
            dao.setStateWithError(message.id, MessageState.FAILED, describe(error))
        }
    }

    /** One outbox attempt for a voice note: only the missing blocks are resent. */
    private suspend fun resendVoice(message: MessageEntity, profile: UserProfile) {
        val path = message.voicePath ?: return
        if (!File(path).exists()) {
            dao.setStateWithError(message.id, MessageState.FAILED, "fichier audio absent")
            outboxAttempts[message.id] = OutboxPolicy.MAX_ATTEMPTS
            return
        }
        val transfer = transferDao.find(message.id)
        // `missing` holds the blocks the receiver asked for again (voice_nack):
        // only those are resent, the rest of the note is never sent twice.
        val missing = BlockSet.decode(transfer?.missing)
        try {
            resumeVoiceTransfer(message, profile, missing.toList())
            outboxAttempts.remove(message.id)
            outboxNextAttempt.remove(message.id)
        } catch (error: ApiException) {
            val reason = if (OutboxPolicy.retryable(error.status)) {
                describe(error)
            } else {
                OutboxPolicy.permanentReason(error.status)
            }
            dao.setStateWithError(message.id, MessageState.FAILED, reason)
        } catch (error: Exception) {
            dao.setStateWithError(message.id, MessageState.FAILED, describe(error))
        }
    }

    private data class DetectedCell(val cell: Cells.Cell, val ip: String)

    private data class StationProbe(val ip: String?, val reportedSite: String?)

    /**
     * Keeps the local station and the WebSocket alive. Cell detection is done by
     * [detectCell]; on failure the precise reason is already in the UI state, so
     * this method simply returns instead of throwing a generic error.
     */
    private suspend fun ensureConnected() {
        val profile = _state.value.profile ?: return
        val detected = detectCell() ?: return
        if (!socket.isConnected()) {
            socket.connect("ws://${detected.ip}:${detected.cell.wsPort}", profile.userId)
        }
        // A real cell change (A -> C) re-registers the user on the new station and
        // resumes the interrupted transfers there. The identity, the conversations
        // and the outbox are untouched: only the routing of this session changes.
        if (boundSite != detected.cell.site) {
            boundSite = detected.cell.site
            outboxResumed = false
            probeFailures = 0
            Log.i(TAG, "session bound to ${detected.cell.site} at ${detected.ip}")
            runCatching { api.registerPresence(profile.userId, profile.displayName) }
            _state.update { it.copy(info = "Cellule ${detected.cell.site} : session rétablie") }
        }
        if (!outboxResumed) {
            outboxResumed = true
            resumeOutbox()
            resumePendingTransfers()
        }
    }

    /**
     * Durable outbox worker. It runs for the whole session and retries every
     * message that a station has not accepted yet (pending, interrupted or
     * failed), with an exponential backoff. Because the queue lives in Room, an
     * application restart resumes it too.
     */
    private fun startOutbox() {
        if (outboxJob?.isActive == true) return
        outboxJob = viewModelScope.launch(Dispatchers.IO) {
            while (isActive) {
                try {
                    if (_state.value.httpConnected) {
                        flushOutbox()
                    }
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    Log.e(TAG, "outbox loop error", error)
                }
                delay(OUTBOX_TICK_MS)
            }
        }
    }

    private suspend fun flushOutbox() {
        val profile = _state.value.profile ?: return
        val now = System.currentTimeMillis()
        val pending = dao.outgoingPending()
        if (pending.isEmpty()) return
        for (message in pending) {
            val attempts = outboxAttempts[message.id] ?: 0
            if (!OutboxPolicy.shouldRetry(attempts)) {
                if (message.state != MessageState.FAILED) {
                    dao.setStateWithError(
                        message.id, MessageState.FAILED,
                        "tentatives épuisées, réessayez manuellement"
                    )
                }
                continue
            }
            val nextAt = outboxNextAttempt[message.id] ?: 0L
            if (now < nextAt) continue
            outboxAttempts[message.id] = attempts + 1
            outboxNextAttempt[message.id] = now + OutboxPolicy.delayFor(attempts)
            when (message.kind) {
                "text" -> resendText(message, profile)
                "voice" -> resendVoice(message, profile)
                else -> Unit
            }
        }
    }

    /** Manual re-check, used by the "Revérifier" and "Autoriser" buttons. */
    fun refreshCell() {
        viewModelScope.launch(Dispatchers.IO) { detectCell() }
    }

    fun onPermissionsResult(granted: Map<String, Boolean>) {
        Log.i(
            TAG,
            "permission result: " +
                granted.entries.joinToString { "${it.key.substringAfterLast('.')}=${it.value}" }
        )
        _state.update { it.copy(missingPermissions = wifi.missingPermissions()) }
        refreshCell()
    }

    /** Wi-Fi callback entry point: debounced because capabilities change in bursts. */
    private fun onNetworkChanged() {
        if (monitorJob?.isActive == true) return
        monitorJob = viewModelScope.launch(Dispatchers.IO) {
            delay(700)
            detectCell()
        }
    }

    private suspend fun detectCell(): DetectedCell? =
        detectionMutex.withLock { detectCellLocked() }

    /**
     * Three-step detection:
     *  1. Wi-Fi transport / enabled state (ConnectivityManager),
     *  2. SSID with an explicit unavailability reason,
     *  3. cell identification by SSID or DHCP gateway, then confirmation by the
     *     station REST API. A value that cannot be read is never guessed.
     */
    private suspend fun detectCellLocked(): DetectedCell? {
        val snapshot = try {
            wifi.snapshot()
        } catch (error: Exception) {
            Log.e(TAG, "cannot read the Wi-Fi state", error)
            _state.update {
                it.copy(
                    cellStatus = CellStatus.UNKNOWN,
                    cellStatusDetail = "Lecture de l'état Wi-Fi impossible : ${describe(error)}",
                    httpConnected = false
                )
            }
            return null
        }
        Log.i(TAG, "wifi snapshot: ${snapshot.describe()}")

        _state.update {
            it.copy(
                wifiConnected = snapshot.connectedToWifi,
                wifiSsid = snapshot.ssid,
                ssidStatus = snapshot.ssidStatus,
                wifiGateway = snapshot.gatewayIp,
                missingPermissions = wifi.missingPermissions(),
                locationServicesEnabled = snapshot.locationServicesEnabled
            )
        }

        if (!snapshot.wifiEnabled) {
            clearStation(CellStatus.NO_WIFI, "Wi-Fi désactivé sur le téléphone")
            return null
        }
        if (!snapshot.connectedToWifi) {
            clearStation(CellStatus.NO_WIFI, "Aucun réseau Wi-Fi actif")
            return null
        }

        val bySsid = Cells.bySsid(snapshot.ssid)
        val byGateway = Cells.byIp(snapshot.gatewayIp)
        val cell = bySsid ?: byGateway
        if (cell == null) {
            val status = when (snapshot.ssidStatus) {
                SsidStatus.NO_PERMISSION -> CellStatus.PERMISSION_MISSING
                SsidStatus.AVAILABLE -> CellStatus.SSID_UNKNOWN
                else -> CellStatus.SSID_UNAVAILABLE
            }
            val detail = when (status) {
                CellStatus.PERMISSION_MISSING ->
                    "Accordez l'accès Wi-Fi et la localisation pour lire le SSID"
                CellStatus.SSID_UNKNOWN ->
                    "Réseau « ${snapshot.ssid} » : ce n'est pas une cellule CellNet"
                else ->
                    "Wi-Fi connecté, SSID illisible " +
                        "(${snapshot.error ?: snapshot.ssidStatus.name}), " +
                        "passerelle ${snapshot.gatewayIp ?: "inconnue"}"
            }
            clearStation(status, detail)
            return null
        }

        val identifiedBy = if (bySsid != null) "SSID" else "PASSERELLE"
        Log.i(TAG, "cell ${cell.site} identified by $identifiedBy")

        // Without Internet Android keeps routing sockets to mobile data, so the
        // ESP32 at 10.x.x.x would never answer: bind the process to the cell.
        if (wifi.bindToActiveWifi() == null) {
            Log.w(TAG, "process not bound to the Wi-Fi network, trying direct routing")
        }

        val probe = resolveStation(cell, snapshot)
        if (probe.ip == null) {
            probeFailures++
            val mismatch = probe.reportedSite
            if (mismatch == null && probeFailures < MAX_PROBE_FAILURES && boundSite == cell.site) {
                // Transient failure: the station is most probably busy (a long
                // transfer fills its egress queue for a few hundred milliseconds).
                // The session, the WebSocket and the pending messages are kept
                // instead of being torn down on a single missed probe.
                Log.w(
                    TAG,
                    "probe failure ${probeFailures}/${MAX_PROBE_FAILURES} on ${cell.site}, " +
                        "session kept"
                )
                _state.update {
                    it.copy(
                        cellSite = cell.site,
                        cellIp = cell.ip,
                        cellStatus = CellStatus.CONNECTED,
                        cellStatusDetail = "Station momentanément occupée, nouvelle tentative…"
                    )
                }
                return null
            }
            _state.update {
                it.copy(
                    cellSite = cell.site,
                    cellIp = cell.ip,
                    cellIdentifiedBy = identifiedBy,
                    stationSite = mismatch,
                    httpConnected = false,
                    wsConnected = false,
                    cellStatus = if (mismatch != null) {
                        CellStatus.STATION_MISMATCH
                    } else {
                        CellStatus.STATION_UNREACHABLE
                    },
                    cellStatusDetail = if (mismatch != null) {
                        "La station répond mais annonce $mismatch au lieu de ${cell.site}"
                    } else {
                        "Aucune réponse de ${cell.ip}:${cell.httpPort}"
                    }
                )
            }
            return null
        }
        probeFailures = 0

        api.baseUrl = "http://${probe.ip}:${cell.httpPort}"
        _state.update {
            it.copy(
                cellSite = cell.site,
                cellIp = probe.ip,
                cellIdentifiedBy = identifiedBy,
                stationSite = probe.reportedSite,
                httpConnected = true,
                wsConnected = socket.isConnected(),
                cellStatus = CellStatus.CONNECTED,
                cellStatusDetail = null,
                error = null
            )
        }
        return DetectedCell(cell, probe.ip)
    }

    /**
     * Tries the configured address first, then the DHCP gateway observed on the
     * Wi-Fi link, and only accepts an answer whose `site` matches the cell. The
     * gateway is therefore never assumed to host the service.
     */
    private suspend fun resolveStation(cell: Cells.Cell, snapshot: WifiSnapshot): StationProbe {
        val candidates = LinkedHashSet<String>()
        candidates.add(cell.ip)
        snapshot.gatewayIp?.let { gateway ->
            if (gateway != cell.ip) candidates.add(gateway)
        }

        var reported: String? = null
        for (ip in candidates) {
            api.baseUrl = "http://$ip:${cell.httpPort}"
            try {
                val info = api.info()
                if (info.site.isBlank()) {
                    Log.w(TAG, "station at $ip answered without a site field")
                    continue
                }
                if (info.site != cell.site) {
                    Log.w(TAG, "station at $ip reports ${info.site}, expected ${cell.site}")
                    reported = info.site
                    continue
                }
                Log.i(TAG, "station ${cell.site} reachable at $ip")
                return StationProbe(ip, info.site)
            } catch (error: Exception) {
                Log.w(
                    TAG,
                    "station ${cell.site} unreachable at $ip: " +
                        "${error.javaClass.simpleName}: ${error.message}"
                )
            }
        }
        return StationProbe(null, reported)
    }

    /** Clears every station-related value so an old cell is never displayed. */
    private fun clearStation(status: CellStatus, detail: String?) {
        socket.disconnect()
        wifi.unbindProcess()
        _state.update {
            it.copy(
                cellSite = null,
                cellIp = null,
                cellIdentifiedBy = null,
                stationSite = null,
                httpConnected = false,
                wsConnected = false,
                cellStatus = status,
                cellStatusDetail = detail
            )
        }
    }

    /**
     * Reopens the transfers that were interrupted by a restart, a lost Wi-Fi link
     * or a cell change. Nothing is deleted: the partial file and the list of
     * validated blocks are both durable, so only the missing blocks are requested
     * again once a station answers.
     */
    private suspend fun restoreTransfers() {
        val now = System.currentTimeMillis()
        transferDao.purgeCompleted(now - TRANSFER_MAX_AGE_MS)
        for (transfer in transferDao.activeIncoming()) {
            if (transfer.kind != "voice") continue
            if (dao.findMessage(transfer.id) == null) {
                transferDao.delete(transfer.id)
                continue
            }
            if (!assembler.partFile(transfer.id).exists()) {
                transferDao.delete(transfer.id)
                continue
            }
            val received = BlockSet.decode(transfer.received)
            assembler.restore(transfer.id, transfer.total, transfer.bytes, transfer.crc, received)
            dao.setReceivedBlocks(transfer.id, received.size)
            pendingResumes[transfer.id] = assembler.missing(transfer.id).toMutableList()
            Log.i(
                TAG,
                "transfer ${transfer.id} restored: ${received.size}/${transfer.total} blocks"
            )
        }
        // Incoming messages left in "receiving" without an active transfer were
        // abandoned: they are removed instead of staying visible for ever.
        val activeIds = transferDao.active().map { it.id }.toSet()
        for (message in dao.receivingMessages()) {
            if (!activeIds.contains(message.id)) {
                dao.deleteMessage(message.id)
                Log.i(TAG, "abandoned incomplete message ${message.id} removed")
            }
        }
    }

    /** Asks again for the missing blocks of the transfers restored at start-up. */
    private suspend fun resumePendingTransfers() {
        if (pendingResumes.isEmpty()) return
        val me = _state.value.profile?.userId ?: return
        for (msgId in pendingResumes.keys.toList()) {
            val missing = pendingResumes[msgId] ?: continue
            val message = dao.findMessage(msgId) ?: continue
            if (message.state != MessageState.RECEIVING || missing.isEmpty()) {
                pendingResumes.remove(msgId)
                continue
            }
            if (message.kind == "voice") {
                val json = JSONObject()
                    .put("kind", "voice_nack")
                    .put("msgId", msgId)
                    .put("from", me)
                    .put("to", message.conversationId)
                    .put("missing", JSONArray(missing))
                runCatching { api.sendVoiceFeedback(json.toString()) }
            } else {
                sendTextNack(msgId, message.conversationId, missing)
            }
            pendingResumes.remove(msgId)
            _state.update { it.copy(info = "Reprise d'un transfert interrompu") }
        }
    }

    /**
     * Immediate replay of the durable outbox at session start (including after an
     * application restart). The periodic worker keeps retrying afterwards with an
     * exponential backoff, and voice notes resume from the blocks the receiver
     * still misses when that information is available.
     */
    private suspend fun resumeOutbox() {
        val pending = dao.outgoingPending()
        if (pending.isEmpty()) return
        outboxAttempts.clear()
        outboxNextAttempt.clear()
        Log.i(TAG, "resuming ${pending.size} message(s) from the durable outbox")
        flushOutbox()
    }

    fun refresh() {
        viewModelScope.launch(Dispatchers.IO) {
            runCatching {
                val users = api.users()
                _state.update { it.copy(users = users) }
            }.onFailure { error ->
                _state.update { it.copy(error = describe(error)) }
            }
        }
    }

    fun requestCell(site: String) {
        val cell = Cells.bySite(site) ?: return
        if (_state.value.wifiSsid.equals(cell.ssid, ignoreCase = true) &&
            _state.value.cellStatus == CellStatus.CONNECTED
        ) {
            _state.update { it.copy(info = "Déjà connecté à $site") }
            refreshCell()
            return
        }
        wifi.connectToCell(cell) { status ->
            Log.i(TAG, "requestCell($site) -> $status")
            _state.update { it.copy(wifiMessage = status) }
            when (status) {
                "connected" -> {
                    refreshCell()
                    startSession()
                }
                "lost" -> clearStation(CellStatus.NO_WIFI, "Wi-Fi perdu")
            }
        }
    }

    fun scanNearbyCells() {
        if (_state.value.scanning) return
        if (wifi.missingPermissions().isNotEmpty()) {
            _state.update { it.copy(info = "Autorisez l'accès Wi-Fi pour scanner les cellules") }
            return
        }
        _state.update { it.copy(scanning = true) }
        viewModelScope.launch(Dispatchers.IO) {
            val started = wifi.scan()
            if (!started) Log.w(TAG, "startScan refused by the system")
            delay(1_500)
            val found = wifi.nearbyCells()
            _state.update { it.copy(scanning = false, nearCells = found) }
        }
    }

    fun clearMessages() {
        // Only the transient information message is consumed: the connection
        // error stays visible until the next successful probe.
        _state.update { it.copy(info = null) }
    }

    // -----------------------------------------------------------------------
    // Conversations
    // -----------------------------------------------------------------------
    fun openConversation(peerId: String, peerName: String) {
        val existing = conversations.value.firstOrNull { it.peerId == peerId }
        val name = peerName.ifBlank { existing?.peerName ?: peerId }
        activePeerFlow.value = peerId
        _state.update { it.copy(activePeer = peerId, activePeerName = name) }
        viewModelScope.launch(Dispatchers.IO) {
            if (dao.findConversation(peerId) == null) {
                dao.upsertConversation(
                    ConversationEntity(
                        peerId = peerId,
                        peerName = name,
                        lastMessageAt = System.currentTimeMillis(),
                        lastPreview = ""
                    )
                )
            }
            dao.clearUnread(peerId)
            val unread = dao.unreadIncoming(peerId)
            for (message in unread) {
                dao.setState(message.id, MessageState.READ)
                sendReceipt(message.id, peerId, "read")
            }
        }
    }

    fun closeConversation() {
        activePeerFlow.value = null
        _state.update { it.copy(activePeer = null, activePeerName = null) }
    }

    private fun directoryUser(peerId: String): DirectoryUser? =
        _state.value.users.firstOrNull { it.id == peerId }

    // -----------------------------------------------------------------------
    // Text messages
    // -----------------------------------------------------------------------
    fun sendText(text: String) {
        val profile = _state.value.profile ?: return
        val peer = _state.value.activePeer ?: return
        val clean = text.trim()
        if (clean.isEmpty()) return

        val bytes = clean.toByteArray(Charsets.UTF_8)
        if (bytes.size > TextChunker.MAX_TEXT_BYTES) {
            _state.update {
                it.copy(
                    error = "Message trop long : ${bytes.size} octets, maximum " +
                        "${TextChunker.MAX_TEXT_BYTES} octets"
                )
            }
            return
        }

        val id = newId()
        val now = System.currentTimeMillis()
        // Short messages keep the single frame path (no behaviour change for the
        // usual case); long ones are chunked so they are no longer limited by the
        // capacity of one bus frame.
        val chunked = TextChunker.needsFragmentation(clean)
        val payload = if (chunked) {
            ""
        } else {
            JSONObject()
                .put("kind", "text")
                .put("id", id)
                .put("from", profile.userId)
                .put("to", peer)
                .put("ts", now)
                .put("body", clean)
                .toString()
        }

        viewModelScope.launch(Dispatchers.IO) {
            dao.insertMessage(
                MessageEntity(
                    id = id,
                    conversationId = peer,
                    fromUser = profile.userId,
                    toUser = peer,
                    kind = "text",
                    body = clean,
                    createdAt = now,
                    state = MessageState.PENDING,
                    isMine = true,
                    voiceTotalBlocks = if (chunked) TextChunker.chunks(clean).size else 0,
                    voiceBytes = bytes.size.toLong()
                )
            )
            touchConversation(peer, clean.take(80), now, incrementUnread = false)
            try {
                if (chunked) {
                    sendTextChunks(id, clean, profile.userId, peer, null)
                } else {
                    deliverText(id, payload)
                }
            } catch (error: Exception) {
                dao.setStateWithError(id, MessageState.FAILED, describe(error))
                _state.update { it.copy(error = "Message non transmis : ${describe(error)}") }
            }
        }
    }

    private suspend fun deliverText(id: String, payload: String) {
        var attempt = 1
        while (attempt <= 3) {
            try {
                dao.setState(id, MessageState.SENDING)
                val response = api.sendText(payload)
                val status = response.optString("status")
                dao.setState(
                    id,
                    if (status == "delivered-local") MessageState.DELIVERED else MessageState.SENT
                )
                return
            } catch (error: Exception) {
                if (attempt >= 3) {
                    dao.setStateWithError(id, MessageState.FAILED, describe(error))
                    _state.update { it.copy(error = "Message non transmis : ${describe(error)}") }
                    return
                }
                attempt++
                delay(1_000L * attempt)
            }
        }
    }

    /**
     * Long text: the body is split into chunks, each sent as its own `text_block`
     * payload (relayed verbatim by the station). The progress is durable, so an
     * interrupted send resumes with the missing chunks only, and a duplicated
     * chunk is ignored by the receiver.
     */
    private suspend fun sendTextChunks(
        id: String,
        body: String,
        from: String,
        to: String,
        only: List<Int>?
    ) {
        val chunks = TextChunker.chunks(body)
        if (chunks.size <= 1) return
        val total = chunks.size
        val bytes = body.toByteArray(Charsets.UTF_8)
        val crc = VoiceProtocol.crc32(bytes)
        val existing = transferDao.find(id)
        transferDao.upsert(
            TransferEntity(
                id = id,
                kind = "text",
                direction = "out",
                peer = to,
                total = total,
                bytes = bytes.size.toLong(),
                crc = crc,
                received = existing?.received ?: "",
                missing = existing?.missing ?: "",
                state = TransferState.ACTIVE,
                updatedAt = System.currentTimeMillis()
            )
        )
        val indexes = only ?: chunks.map { it.index }
        _state.update { it.copy(sendingMessage = id, sendProgress = 0f) }
        var sent = 0
        for (index in indexes) {
            val chunk = chunks.getOrNull(index) ?: continue
            val payload = JSONObject()
                .put("kind", "text_block")
                .put("id", id)
                .put("from", from)
                .put("to", to)
                .put("ts", System.currentTimeMillis())
                .put("seq", chunk.index)
                .put("total", chunk.total)
                .put("bytes", bytes.size)
                .put("crc", crc)
                .put("body", Base64.encodeToString(chunk.data, Base64.NO_WRAP))
                .toString()
            retry(3) { api.sendText(payload) }
            sent++
            _state.update { it.copy(sendProgress = sent.toFloat() / indexes.size) }
        }
        dao.setState(id, MessageState.SENT)
        transferDao.setState(id, TransferState.COMPLETE, System.currentTimeMillis())
        _state.update {
            it.copy(
                sendingMessage = null,
                sendProgress = 0f,
                info = if (only == null) {
                    "Message long transmis en $total blocs"
                } else {
                    "Blocs manquants renvoyés (${indexes.size})"
                }
            )
        }
    }

    /**
     * A chunk of a long text arrived. The message is only published (and the
     * `delivered` receipt only sent) once every chunk is present: a partial
     * transfer is never presented as a complete message.
     */
    private suspend fun onIncomingTextBlock(json: JSONObject) {
        val me = _state.value.profile?.userId ?: return
        val id = json.optString("id")
        val from = json.optString("from")
        val to = json.optString("to")
        val index = json.optInt("seq", -1)
        val total = json.optInt("total", 0)
        val bytes = json.optLong("bytes", 0L)
        val crc = json.optLong("crc", 0L)
        val body = json.optString("body")
        if (id.isBlank() || from.isBlank() || from == me) return
        if (to.isNotBlank() && to != me) return
        if (total <= 1 || index < 0 || index >= total || body.isBlank()) return

        val data = runCatching { Base64.decode(body, Base64.NO_WRAP) }.getOrNull() ?: return
        val existing = dao.findMessage(id)
        if (existing != null && existing.state != MessageState.RECEIVING) {
            sendReceipt(id, from, "delivered")  // already complete: stop the retries
            return
        }
        if (existing == null) {
            dao.insertMessage(
                MessageEntity(
                    id = id,
                    conversationId = from,
                    fromUser = from,
                    toUser = me,
                    kind = "text",
                    body = "",
                    createdAt = System.currentTimeMillis(),
                    state = MessageState.RECEIVING,
                    isMine = false,
                    voiceTotalBlocks = total,
                    voiceBytes = bytes,
                    voiceCrc = crc
                )
            )
            touchConversation(
                from, "Message long en réception", System.currentTimeMillis(),
                activePeerFlow.value != from, directoryUser(from)?.name
            )
        }
        val transfer = transferDao.find(id) ?: TransferEntity(
            id = id, kind = "text", direction = "in", peer = from, total = total,
            bytes = bytes, crc = crc, received = "", missing = "",
            state = TransferState.ACTIVE, updatedAt = System.currentTimeMillis()
        )
        val received = BlockSet.decode(transfer.received)
        if (!received.add(index)) return  // duplicate chunk: idempotent
        textAssembler.add(id, index, total, data)
        transferDao.upsert(
            transfer.copy(
                received = BlockSet.encode(received),
                updatedAt = System.currentTimeMillis()
            )
        )
        dao.setReceivedBlocks(id, received.size)
        if (received.size >= total) {
            completeTextTransfer(id, from)
        } else {
            scheduleTransferWatchdog(id, from)
        }
    }

    private suspend fun completeTextTransfer(id: String, peer: String) {
        val message = dao.findMessage(id) ?: return
        val assembled = textAssembler.assemble(id)
        if (assembled == null) {
            val missing = textAssembler.missing(id)
            if (missing.isNotEmpty()) sendTextNack(id, peer, missing)
            return
        }
        dao.updateMessage(
            message.copy(
                body = assembled,
                state = MessageState.RECEIVED,
                voiceReceivedBlocks = message.voiceTotalBlocks
            )
        )
        transferDao.setState(id, TransferState.COMPLETE, System.currentTimeMillis())
        textAssembler.discard(id)
        touchConversation(
            peer, assembled.take(80), message.createdAt,
            activePeerFlow.value != peer, directoryUser(peer)?.name
        )
        sendReceipt(id, peer, "delivered")
        _state.update { it.copy(info = "Message long reçu (${assembled.length} caractères)") }
    }

    /** Asks the sender for the chunks that are still missing. */
    private suspend fun sendTextNack(id: String, peer: String, missing: List<Int>) {
        val me = _state.value.profile?.userId ?: return
        val json = JSONObject()
            .put("kind", "text_nack")
            .put("id", id)
            .put("from", me)
            .put("to", peer)
            .put("missing", JSONArray(missing))
            .toString()
        runCatching { api.sendText(json) }
    }

    private suspend fun onTextNack(json: JSONObject) {
        val me = _state.value.profile?.userId ?: return
        val id = json.optString("id")
        if (id.isBlank()) return
        val message = dao.findMessage(id) ?: return
        if (!message.isMine) return
        val array = json.optJSONArray("missing") ?: return
        val indexes = (0 until array.length()).map { array.optInt(it) }
            .filter { it >= 0 }
            .distinct()
        if (indexes.isEmpty()) return
        runCatching {
            sendTextChunks(id, message.body, me, message.conversationId, indexes)
        }.onFailure { error ->
            _state.update {
                it.copy(error = "Reprise du message long impossible : ${describe(error)}")
            }
        }
    }

    private suspend fun onIncomingText(json: JSONObject) {
        val me = _state.value.profile?.userId ?: return
        val id = json.optString("id")
        val from = json.optString("from")
        val to = json.optString("to")
        val body = json.optString("body")
        val ts = json.optLong("ts", System.currentTimeMillis())
        if (id.isBlank() || from.isBlank() || from == me) return
        if (to.isNotBlank() && to != me) return

        val inserted = dao.insertMessage(
            MessageEntity(
                id = id,
                conversationId = from,
                fromUser = from,
                toUser = me,
                kind = "text",
                body = body,
                createdAt = ts,
                state = MessageState.RECEIVED,
                isMine = false
            )
        )
        if (inserted == -1L) return  // duplicate frame, already stored

        touchConversation(
            peerId = from,
            preview = body,
            at = ts,
            incrementUnread = activePeerFlow.value != from,
            peerNameOverride = directoryUser(from)?.name
        )
        sendReceipt(id, from, "delivered")
    }

    private suspend fun onReceipt(json: JSONObject) {
        val me = _state.value.profile?.userId ?: return
        val id = json.optString("id")
        if (id.isBlank()) return
        val to = json.optString("to")
        if (to.isNotBlank() && to != me) return
        val message = dao.findMessage(id) ?: return
        if (!message.isMine) return
        when (json.optString("state")) {
            "delivered" -> if (message.state != MessageState.READ) {
                dao.setState(id, MessageState.DELIVERED)
            }
            "read" -> dao.setState(id, MessageState.READ)
        }
    }

    private suspend fun sendReceipt(messageId: String, peer: String, state: String) {
        val me = _state.value.profile?.userId ?: return
        val json = JSONObject()
            .put("kind", "receipt")
            .put("id", messageId)
            .put("from", me)
            .put("to", peer)
            .put("state", state)
            .put("ts", System.currentTimeMillis())
            .toString()
        runCatching { api.sendReceipt(json) }
    }

    private suspend fun touchConversation(
        peerId: String,
        preview: String,
        at: Long,
        incrementUnread: Boolean,
        peerNameOverride: String? = null
    ) {
        val current = dao.findConversation(peerId)
        val directory = directoryUser(peerId)
        val name = peerNameOverride?.takeIf { it.isNotBlank() }
            ?: current?.peerName
            ?: directory?.name
            ?: peerId
        dao.upsertConversation(
            ConversationEntity(
                peerId = peerId,
                peerName = name,
                station = directory?.station ?: current?.station,
                lastMessageAt = at,
                lastPreview = preview.take(80),
                unread = (current?.unread ?: 0) + if (incrementUnread) 1 else 0
            )
        )
    }

    // -----------------------------------------------------------------------
    // Voice messages
    // -----------------------------------------------------------------------
    fun startRecording() {
        if (recorder.isRecording) return
        if (_state.value.activePeer == null) return
        val started = runCatching { recorder.start() }.isSuccess
        if (!started) {
            _state.update {
                it.copy(error = "Micro indisponible : vérifiez la permission d'enregistrement")
            }
            return
        }
        _state.update { it.copy(recording = true, recordingMs = 0L, error = null) }
        recordingJob = viewModelScope.launch {
            while (isActive && _state.value.recording) {
                val elapsed = recorder.elapsedMs()
                _state.update { it.copy(recordingMs = elapsed) }
                if (elapsed >= VoiceProtocol.MAX_DURATION_MS) {
                    stopRecordingAndSend()
                    return@launch
                }
                delay(100)
            }
        }
    }

    fun cancelRecording() {
        recordingJob?.cancel()
        recordingJob = null
        recorder.cancel()
        _state.update { it.copy(recording = false, recordingMs = 0L) }
    }

    fun stopRecordingAndSend() {
        if (!recorder.isRecording) return
        val clip = recorder.stop()
        _state.update { it.copy(recording = false, recordingMs = 0L) }
        if (clip == null) {
            _state.update { it.copy(error = "Enregistrement vide") }
            return
        }
        viewModelScope.launch(Dispatchers.IO) { sendVoiceClip(clip) }
    }

    private suspend fun sendVoiceClip(clip: VoiceRecorder.Clip) {
        val profile = _state.value.profile ?: return
        val peer = _state.value.activePeer ?: return
        val bytes = runCatching { clip.file.readBytes() }.getOrNull() ?: return
        val crc = VoiceProtocol.crc32(bytes)
        val total = VoiceProtocol.blockCount(bytes.size.toLong())
        if (total <= 0) return

        val msgId = newId()
        val now = System.currentTimeMillis()
        val entity = MessageEntity(
            id = msgId,
            conversationId = peer,
            fromUser = profile.userId,
            toUser = peer,
            kind = "voice",
            body = "",
            createdAt = now,
            state = MessageState.SENDING,
            isMine = true,
            voicePath = clip.file.absolutePath,
            voiceDurationMs = clip.durationMs,
            voiceBytes = bytes.size.toLong(),
            voiceTotalBlocks = total,
            voiceCrc = crc
        )
        dao.insertMessage(entity)
        touchConversation(peer, "Message vocal", now, incrementUnread = false)

        try {
            resumeVoiceTransfer(entity, profile, emptyList())
        } catch (error: Exception) {
            dao.setStateWithError(msgId, MessageState.FAILED, describe(error))
            _state.update {
                it.copy(
                    sendingMessage = null,
                    sendProgress = 0f,
                    error = "Envoi vocal interrompu : ${describe(error)}"
                )
            }
        }
    }

    /**
     * Sends (or resumes) a voice note. `only` lists the blocks the receiver asked
     * for again: an interrupted transfer of 500 blocks restarts from the 420 that
     * were already validated instead of starting over.
     */
    private suspend fun resumeVoiceTransfer(
        message: MessageEntity,
        profile: UserProfile,
        only: List<Int>
    ) {
        val path = message.voicePath ?: return
        val bytes = runCatching { File(path).readBytes() }.getOrNull() ?: return
        val total = message.voiceTotalBlocks
        if (total <= 0) return
        val existing = transferDao.find(message.id)
        transferDao.upsert(
            TransferEntity(
                id = message.id,
                kind = "voice",
                direction = "out",
                peer = message.conversationId,
                total = total,
                bytes = bytes.size.toLong(),
                crc = message.voiceCrc,
                received = existing?.received ?: "",
                missing = existing?.missing ?: "",
                state = TransferState.ACTIVE,
                updatedAt = System.currentTimeMillis()
            )
        )
        _state.update { it.copy(sendingMessage = message.id, sendProgress = 0f) }
        api.sendVoiceFeedback(
            feedbackJson(
                "voice_begin", message.id, profile.userId, message.conversationId,
                mapOf(
                    "total" to total,
                    "bytes" to bytes.size,
                    "durationMs" to message.voiceDurationMs,
                    "crc" to message.voiceCrc,
                    "mime" to VoiceProtocol.MIME,
                    "resume" to only.isNotEmpty()
                )
            )
        )
        uploadBlocks(
            message.id, bytes, total, profile.userId, message.conversationId,
            if (only.isEmpty()) null else only
        )
        dao.setState(message.id, MessageState.SENT)
        _state.update {
            it.copy(
                sendingMessage = null,
                sendProgress = 0f,
                info = if (only.isEmpty()) {
                    "Vocal transmis, en attente de confirmation du destinataire"
                } else {
                    "Blocs manquants renvoyés (${only.size})"
                }
            )
        }
    }

    private suspend fun uploadBlocks(
        msgId: String,
        bytes: ByteArray,
        total: Int,
        from: String,
        to: String,
        only: List<Int>?
    ) {
        val indexes = only ?: (0 until total).toList()
        for (seq in indexes) {
            val start = seq * VoiceProtocol.BLOCK_SIZE
            val end = minOf(bytes.size, start + VoiceProtocol.BLOCK_SIZE)
            if (start >= end) continue
            val chunk = bytes.copyOfRange(start, end)
            val payload = JSONObject()
                .put("msgId", msgId)
                .put("from", from)
                .put("to", to)
                .put("seq", seq)
                .put("total", total)
                .put("crc", crc16(chunk))
                .put("data", Base64.encodeToString(chunk, Base64.NO_WRAP))
                .toString()
            retry(3) { api.sendVoiceBlock(payload) }
            if (only == null) {
                _state.update { it.copy(sendProgress = (seq + 1).toFloat() / total) }
            }
        }
    }

    private suspend fun <T> retry(attempts: Int, block: suspend () -> T): T {
        var last: Exception? = null
        for (index in 0 until attempts) {
            try {
                return block()
            } catch (error: Exception) {
                last = error
                delay(400L * (index + 1))
            }
        }
        throw last ?: IllegalStateException("retry failed")
    }

    // -----------------------------------------------------------------------
    // Voice reception
    // -----------------------------------------------------------------------
    private fun handleVoiceBlockData(bytes: ByteArray) {
        // The binary frame identifies its own block (magic + msgId + seq + total).
        // The JSON header is only used as a fallback for a station that still
        // pushes the previous, unversioned format.
        val frame = BinaryVoiceFrame.decode(bytes)
        val msgId: String
        val seq: Int
        if (frame != null) {
            msgId = frame.msgId
            seq = frame.seq
            pendingVoiceBlock = null
        } else {
            val pending = pendingVoiceBlock ?: return
            pendingVoiceBlock = null
            msgId = pending.first
            seq = pending.second
        }
        if (msgId.isBlank() || seq < 0) return
        viewModelScope.launch(Dispatchers.IO) {
            val progress = assembler.add(msgId, seq, if (frame != null) frame.data else bytes)
                ?: return@launch
            dao.setReceivedBlocks(msgId, progress.received)
            val transfer = transferDao.find(msgId)
            if (transfer != null) {
                transferDao.upsert(
                    transfer.copy(
                        received = BlockSet.encode(assembler.receivedIndexes(msgId)),
                        updatedAt = System.currentTimeMillis()
                    )
                )
            }
            if (progress.received >= progress.total) {
                completeVoiceTransfer(msgId)
            }
        }
    }

    private suspend fun completeVoiceTransfer(msgId: String) {
        val message = dao.findMessage(msgId) ?: return
        val me = _state.value.profile?.userId ?: return
        val result = assembler.finish(msgId)
        if (result.ok && result.file != null) {
            dao.completeVoice(msgId, result.file.absolutePath, MessageState.RECEIVED)
            transferDao.setState(msgId, TransferState.COMPLETE, System.currentTimeMillis())
            sendFeedback("voice_done", msgId, me, message.conversationId)
        } else {
            dao.setStateWithError(msgId, MessageState.FAILED, result.error ?: "transfert incomplet")
            if (result.missing.isNotEmpty()) {
                val json = JSONObject()
                    .put("kind", "voice_nack")
                    .put("msgId", msgId)
                    .put("from", me)
                    .put("to", message.conversationId)
                    .put("missing", JSONArray(result.missing))
                runCatching { api.sendVoiceFeedback(json.toString()) }
            }
        }
    }

    private suspend fun onVoiceControl(json: JSONObject) {
        val kind = json.optString("kind")
        val msgId = json.optString("msgId")
        val from = json.optString("from")
        val to = json.optString("to")
        val me = _state.value.profile?.userId ?: return
        if (msgId.isBlank()) return

        when (kind) {
            "voice_begin" -> {
                if (to.isNotBlank() && to != me) return
                val total = json.optInt("total")
                val bytes = json.optLong("bytes")
                val crc = json.optLong("crc")
                val duration = json.optLong("durationMs")
                if (total <= 0 || bytes <= 0L) return
                val resuming = json.optBoolean("resume", false)
                val known = dao.findMessage(msgId)
                if (known != null && known.state == MessageState.RECEIVED) {
                    // The note is already complete: re-acknowledge it so the sender
                    // stops retrying, and never rebuild it from scratch.
                    sendFeedback("voice_done", msgId, me, from)
                    return
                }
                if (known == null) {
                    dao.insertMessage(
                        MessageEntity(
                            id = msgId,
                            conversationId = from,
                            fromUser = from,
                            toUser = me,
                            kind = "voice",
                            createdAt = System.currentTimeMillis(),
                            state = MessageState.RECEIVING,
                            isMine = false,
                            voiceTotalBlocks = total,
                            voiceBytes = bytes,
                            voiceDurationMs = duration,
                            voiceCrc = crc
                        )
                    )
                    touchConversation(
                        from, "Message vocal", System.currentTimeMillis(),
                        activePeerFlow.value != from, directoryUser(from)?.name
                    )
                }
                val previous = transferDao.find(msgId)
                val received = BlockSet.decode(previous?.received)
                if (resuming && received.isNotEmpty()) {
                    // Resume: only the blocks that were never validated will be
                    // asked for again by the watchdog.
                    assembler.restore(msgId, total, bytes, crc, received)
                } else {
                    assembler.begin(msgId, total, bytes, crc)
                }
                transferDao.upsert(
                    TransferEntity(
                        id = msgId,
                        kind = "voice",
                        direction = "in",
                        peer = from,
                        total = total,
                        bytes = bytes,
                        crc = crc,
                        received = if (resuming) BlockSet.encode(received) else "",
                        missing = previous?.missing ?: "",
                        state = TransferState.ACTIVE,
                        updatedAt = System.currentTimeMillis()
                    )
                )
                dao.setReceivedBlocks(msgId, assembler.receivedIndexes(msgId).size)
                scheduleTransferWatchdog(msgId, from)
            }
            "voice_nack" -> {
                val message = dao.findMessage(msgId) ?: return
                if (!message.isMine) return
                val array = json.optJSONArray("missing") ?: return
                val indexes = (0 until array.length()).map { array.optInt(it) }
                    .filter { it >= 0 }
                    .distinct()
                if (indexes.isEmpty()) return
                val profile = _state.value.profile ?: return
                // The list is persisted: after a restart the outbox resends exactly
                // these blocks instead of the whole voice note.
                transferDao.setMissing(
                    msgId, BlockSet.encode(indexes), System.currentTimeMillis()
                )
                runCatching { resumeVoiceTransfer(message, profile, indexes) }
                    .onFailure { error ->
                        _state.update {
                            it.copy(error = "Reprise du vocal impossible : ${describe(error)}")
                        }
                    }
            }
            "voice_done" -> {
                val message = dao.findMessage(msgId) ?: return
                if (message.isMine && message.state != MessageState.READ) {
                    dao.setState(msgId, MessageState.DELIVERED)
                }
            }
            "voice_played" -> {
                val message = dao.findMessage(msgId) ?: return
                if (message.isMine) dao.setState(msgId, MessageState.READ)
            }
        }
    }

    /**
     * Watchdog of an incoming transfer (voice note or long text): when no block
     * arrives any more, the missing ones are requested again. It never declares a
     * transfer complete, it only asks for what is still absent.
     */
    private fun scheduleTransferWatchdog(msgId: String, peer: String) {
        viewModelScope.launch(Dispatchers.IO) {
            delay(TRANSFER_WATCHDOG_MS)
            val message = dao.findMessage(msgId) ?: return@launch
            if (message.state != MessageState.RECEIVING) return@launch
            val me = _state.value.profile?.userId ?: return@launch
            val transfer = transferDao.find(msgId) ?: return@launch
            when (transfer.kind) {
                "voice" -> {
                    val missing = assembler.missing(msgId)
                    if (missing.isEmpty()) return@launch
                    val json = JSONObject()
                        .put("kind", "voice_nack")
                        .put("msgId", msgId)
                        .put("from", me)
                        .put("to", peer)
                        .put("missing", JSONArray(missing))
                    runCatching { api.sendVoiceFeedback(json.toString()) }
                        .onSuccess {
                            Log.i(TAG, "voice $msgId: asked for ${missing.size} missing blocks")
                        }
                }
                "text" -> {
                    val missing = textAssembler.missing(msgId)
                    if (missing.isEmpty()) return@launch
                    sendTextNack(msgId, peer, missing)
                }
                else -> Unit
            }
        }
    }

    fun playVoice(message: MessageEntity) {
        val path = message.voicePath ?: return
        if (player.playingMessageId() == message.id) {
            player.stop()
            _state.update { it.copy(playingMessage = null) }
            return
        }
        _state.update { it.copy(playingMessage = message.id) }
        player.play(message.id, File(path)) { finished ->
            viewModelScope.launch(Dispatchers.IO) { onVoicePlayed(finished) }
        }
    }

    private suspend fun onVoicePlayed(id: String) {
        _state.update { it.copy(playingMessage = null) }
        val message = dao.findMessage(id) ?: return
        if (message.isMine) return
        dao.setPlayed(id, System.currentTimeMillis())
        val me = _state.value.profile?.userId ?: return
        sendFeedback("voice_played", id, me, message.conversationId)
    }

    private suspend fun sendFeedback(
        kind: String,
        msgId: String,
        from: String,
        to: String,
        extra: Map<String, Any?> = emptyMap()
    ) {
        runCatching { api.sendVoiceFeedback(feedbackJson(kind, msgId, from, to, extra)) }
            .onFailure { error ->
                _state.update { it.copy(error = "Retour vocal non transmis : ${describe(error)}") }
            }
    }

    private fun feedbackJson(
        kind: String,
        msgId: String,
        from: String,
        to: String,
        extra: Map<String, Any?> = emptyMap()
    ): String {
        val json = JSONObject()
            .put("kind", kind)
            .put("msgId", msgId)
            .put("from", from)
            .put("to", to)
            .put("ts", System.currentTimeMillis())
        for ((key, value) in extra) {
            json.put(key, value)
        }
        return json.toString()
    }

    // -----------------------------------------------------------------------
    // Socket dispatch
    // -----------------------------------------------------------------------
    private fun handleSocketText(text: String) {
        val json = runCatching { JSONObject(text) }.getOrNull() ?: return
        // The station forwards text/receipt bodies verbatim (they use "kind"),
        // its own events use "type". Accept both.
        val type = json.optString("type").ifBlank { json.optString("kind") }
        when (type) {
            "hello_ack" -> {
                _state.update { it.copy(wsConnected = true, lastDisconnect = null) }
                applyDirectory(json.optJSONArray("users"))
            }
            "presence" -> applyDirectory(json.optJSONArray("users"))
            "text" -> viewModelScope.launch(Dispatchers.IO) { onIncomingText(json) }
            "text_block" -> viewModelScope.launch(Dispatchers.IO) { onIncomingTextBlock(json) }
            "text_nack" -> viewModelScope.launch(Dispatchers.IO) { onTextNack(json) }
            "receipt" -> viewModelScope.launch(Dispatchers.IO) { onReceipt(json) }
            "voice_block" -> {
                // Informative only: the binary frame that follows carries its own
                // identity, so a lost header can never mix two voice notes.
                val msgId = json.optString("msgId")
                val seq = json.optInt("seq", -1)
                if (msgId.isNotBlank() && seq >= 0) {
                    pendingVoiceBlock = msgId to seq
                }
            }
            "voice_begin", "voice_nack", "voice_done", "voice_played" ->
                viewModelScope.launch(Dispatchers.IO) { onVoiceControl(json) }
        }
    }

    private fun applyDirectory(array: JSONArray?) {
        if (array == null) return
        val users = (0 until array.length()).mapNotNull { index ->
            val item = array.optJSONObject(index) ?: return@mapNotNull null
            val id = item.optString("id")
            if (id.isBlank()) return@mapNotNull null
            DirectoryUser(
                id = id,
                name = item.optString("name", id),
                station = item.optString("station", ""),
                online = item.optBoolean("online", false)
            )
        }
        _state.update { it.copy(users = users) }
        viewModelScope.launch(Dispatchers.IO) {
            users.filter { it.station.isNotBlank() }.forEach {
                dao.setConversationStation(it.id, it.station)
            }
        }
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    private fun newId(): String = "m-" + UUID.randomUUID().toString().replace("-", "").take(14)

    private fun describe(error: Throwable): String = when (error) {
        is ApiException -> error.status + (error.detail?.let { " — $it" } ?: "")
        else -> error.message ?: error.javaClass.simpleName
    }

    private fun crc16(data: ByteArray): Int {
        var crc = 0xFFFF
        for (byte in data) {
            crc = crc xor ((byte.toInt() and 0xFF) shl 8)
            repeat(8) {
                crc = if ((crc and 0x8000) != 0) {
                    ((crc shl 1) xor 0x1021) and 0xFFFF
                } else {
                    (crc shl 1) and 0xFFFF
                }
            }
        }
        return crc and 0xFFFF
    }

    override fun onCleared() {
        wifi.stopMonitoring()
        wifi.release()
        socket.disconnect()
        player.stop()
        recorder.cancel()
        sessionJob?.cancel()
        outboxJob?.cancel()
        super.onCleared()
    }

    companion object {
        private const val TAG = "CellNetSession"

        /** Period of the durable outbox worker. */
        private const val OUTBOX_TICK_MS = 3_000L

        /** Silence after which an incomplete transfer asks for its missing blocks. */
        private const val TRANSFER_WATCHDOG_MS = 12_000L

        /** Partial files older than this are considered abandoned and removed. */
        private const val TRANSFER_MAX_AGE_MS = 24L * 3600L * 1000L

        /** Consecutive probe failures tolerated before the session is dropped. */
        private const val MAX_PROBE_FAILURES = 3
    }
}
