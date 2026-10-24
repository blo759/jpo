package com.jpo.cellnet.call

/** Phase of a live call, as displayed by the interface. */
enum class CallPhase {
    /** No call at all. */
    IDLE,

    /** Outgoing invitation sent, waiting for the other party. */
    DIALING,

    /** Incoming invitation received, waiting for the user. */
    RINGING,

    /** Both parties accepted: the media channel is open. */
    ACTIVE,

    /** Terminal phase, only kept long enough to display the reason. */
    ENDED
}

/** Why a call was refused before it even started. */
enum class CallRefusal {
    NONE,
    BUSY,
    NO_CELL,
    SELF,
    UNKNOWN_USER,

    /**
     * The recipient is registered on a station that is not part of the
     * deployment, so there is no way to route the call to it.
     */
    UNKNOWN_CELL,
    NOT_CONNECTED
}

/**
 * Client side guard of the call feature.
 *
 * The station is authoritative (it decides whether the recipient is reachable
 * and refuses a call it cannot route), but the application must never let the
 * user start a call that cannot work: it checks the *local* station and the cell
 * the peer is registered on before sending anything.
 *
 * Calls are allowed between any two cells of the deployment: when the peer is on
 * another cell, the station routes the signalling and the audio over the Wi-Fi
 * backbone. Only a peer whose cell is not part of the deployment is refused.
 *
 * Pure Kotlin: unit tested on the JVM without Android.
 */
object CallSessionPolicy {

    fun canStart(
        phase: CallPhase,
        localCell: String?,
        peerStation: String?,
        peerId: String,
        selfId: String,
        stationConnected: Boolean,
        knownCells: Set<String> = emptySet()
    ): CallRefusal {
        if (phase != CallPhase.IDLE) return CallRefusal.BUSY
        if (peerId.isBlank() || peerId == selfId) {
            return if (peerId == selfId) CallRefusal.SELF else CallRefusal.UNKNOWN_USER
        }
        if (localCell.isNullOrBlank()) return CallRefusal.NO_CELL
        if (!stationConnected) return CallRefusal.NOT_CONNECTED
        if (peerStation.isNullOrBlank()) return CallRefusal.UNKNOWN_USER
        // The peer may be on this cell or on another one: both are allowed. A
        // station outside the deployment, however, cannot be reached at all.
        if (knownCells.isNotEmpty() && knownCells.none { it.equals(peerStation, ignoreCase = true) }) {
            return CallRefusal.UNKNOWN_CELL
        }
        return CallRefusal.NONE
    }

    /** True when the two users are not on the same cell (the call crosses cells). */
    fun isCrossCell(localCell: String?, peerStation: String?): Boolean {
        if (localCell.isNullOrBlank() || peerStation.isNullOrBlank()) return false
        return !peerStation.equals(localCell, ignoreCase = true)
    }

    /**
     * Applies a station event to the current phase. An event that does not belong
     * to the current call is ignored (`callId` filtering is done by the caller),
     * so a late event can never resurrect a finished call.
     */
    fun applyEvent(phase: CallPhase, event: String, isCaller: Boolean): CallPhase = when (event) {
        "call_ringing" -> if (phase == CallPhase.DIALING) CallPhase.DIALING else phase
        "call_invite" -> if (phase == CallPhase.IDLE && !isCaller) CallPhase.RINGING else phase
        "call_accepted" -> if (phase == CallPhase.DIALING || phase == CallPhase.RINGING) {
            CallPhase.ACTIVE
        } else {
            phase
        }
        "call_rejected", "call_cancelled", "call_ended", "call_timeout" -> when (phase) {
            CallPhase.IDLE -> phase
            else -> CallPhase.ENDED
        }
        else -> phase
    }

    fun isTerminal(phase: CallPhase): Boolean = phase == CallPhase.ENDED

    /** A call that reached ACTIVE can be hung up; a ringing one can be cancelled. */
    fun canHangUp(phase: CallPhase): Boolean = phase == CallPhase.ACTIVE

    fun canCancel(phase: CallPhase): Boolean = phase == CallPhase.DIALING

    fun canAnswer(phase: CallPhase): Boolean = phase == CallPhase.RINGING

    fun refusalMessage(refusal: CallRefusal): String = when (refusal) {
        CallRefusal.NONE -> ""
        CallRefusal.BUSY -> "Un appel est déjà en cours"
        CallRefusal.NO_CELL -> "Connectez le téléphone à une cellule pour appeler"
        CallRefusal.SELF -> "Vous ne pouvez pas vous appeler vous-même"
        CallRefusal.UNKNOWN_USER -> "Destinataire inconnu : ouvrez son application"
        CallRefusal.UNKNOWN_CELL -> "Cellule du destinataire inconnue"
        CallRefusal.NOT_CONNECTED -> "Station injoignable : appel impossible"
    }

    /** Human readable reason of an ended call, taken from the station event. */
    fun endReasonLabel(reason: String?): String = when (reason) {
        "rejected" -> "Appel refusé"
        "cancelled" -> "Appel annulé"
        "hangup" -> "Appel terminé"
        "timeout" -> "Appel terminé (délai dépassé)"
        "disconnected" -> "Appel terminé (correspondant déconnecté)"
        "busy" -> "Correspondant occupé"
        "unavailable" -> "Correspondant indisponible"
        "cross-cell" -> "Cellule du correspondant injoignable"
        "cell-unreachable" -> "Liaison Wi-Fi entre stations indisponible"
        "link-unavailable" -> "Liaison Wi-Fi entre stations indisponible"
        null, "" -> "Appel terminé"
        else -> "Appel terminé ($reason)"
    }
}
