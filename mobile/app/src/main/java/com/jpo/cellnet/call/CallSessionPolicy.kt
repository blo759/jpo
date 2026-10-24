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
    CROSS_CELL,
    NOT_CONNECTED
}

/**
 * Client side guard of the call feature.
 *
 * The station is authoritative (it refuses a cross cell call even if the
 * application asks for one), but the application must never let the user start a
 * call that cannot work: it checks the *local* cell against the station the peer
 * is registered on before sending anything.
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
        stationConnected: Boolean
    ): CallRefusal {
        if (phase != CallPhase.IDLE) return CallRefusal.BUSY
        if (peerId.isBlank() || peerId == selfId) {
            return if (peerId == selfId) CallRefusal.SELF else CallRefusal.UNKNOWN_USER
        }
        if (localCell.isNullOrBlank()) return CallRefusal.NO_CELL
        if (!stationConnected) return CallRefusal.NOT_CONNECTED
        if (peerStation.isNullOrBlank()) return CallRefusal.UNKNOWN_USER
        if (!peerStation.equals(localCell, ignoreCase = true)) return CallRefusal.CROSS_CELL
        return CallRefusal.NONE
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
        CallRefusal.UNKNOWN_USER -> "Destinataire inconnu dans cette cellule"
        CallRefusal.CROSS_CELL ->
            "Les appels en direct sont limités aux utilisateurs de votre cellule"
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
        null, "" -> "Appel terminé"
        else -> "Appel terminé ($reason)"
    }
}
