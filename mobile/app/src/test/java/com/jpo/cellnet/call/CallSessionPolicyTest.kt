package com.jpo.cellnet.call

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Client side guard of the live call feature.
 *
 * The station is authoritative (it refuses a cross cell call server side), but
 * the application must never let the user start a call that cannot work. These
 * tests cover the three allowed cells, the six forbidden cross cell pairs and
 * the busy / identity / connection cases.
 */
class CallSessionPolicyTest {

    private val deployment = setOf("SITE_A", "SITE_B", "SITE_C")

    private fun canStart(
        peerId: String = "u-2",
        peerStation: String? = "SITE_A",
        localCell: String? = "SITE_A",
        selfId: String = "u-1",
        phase: CallPhase = CallPhase.IDLE,
        connected: Boolean = true,
        knownCells: Set<String> = deployment
    ) = CallSessionPolicy.canStart(
        phase, localCell, peerStation, peerId, selfId, connected, knownCells
    )

    // -----------------------------------------------------------------------
    // Allowed: both users on the same cell
    // -----------------------------------------------------------------------
    @Test
    fun callBetweenTwoUsersOfSiteAIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-a2", peerStation = "SITE_A", localCell = "SITE_A", selfId = "u-a1")
        )
    }

    @Test
    fun callBetweenTwoUsersOfSiteBIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-b2", peerStation = "SITE_B", localCell = "SITE_B", selfId = "u-b1")
        )
    }

    @Test
    fun callBetweenTwoUsersOfSiteCIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-c2", peerStation = "SITE_C", localCell = "SITE_C", selfId = "u-c1")
        )
    }

    @Test
    fun cellComparisonIgnoresCase() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-a2", peerStation = "site_a", localCell = "SITE_A", selfId = "u-a1")
        )
    }

    // -----------------------------------------------------------------------
    // Allowed: the two users are on different cells of the deployment
    // -----------------------------------------------------------------------
    @Test
    fun callFromSiteAToSiteBIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-b1", peerStation = "SITE_B", localCell = "SITE_A", selfId = "u-a1")
        )
    }

    @Test
    fun callFromSiteAToSiteCIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-c1", peerStation = "SITE_C", localCell = "SITE_A", selfId = "u-a1")
        )
    }

    @Test
    fun callFromSiteBToSiteAIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-a1", peerStation = "SITE_A", localCell = "SITE_B", selfId = "u-b1")
        )
    }

    @Test
    fun callFromSiteBToSiteCIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-c1", peerStation = "SITE_C", localCell = "SITE_B", selfId = "u-b1")
        )
    }

    @Test
    fun callFromSiteCToSiteAIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-a1", peerStation = "SITE_A", localCell = "SITE_C", selfId = "u-c1")
        )
    }

    @Test
    fun callFromSiteCToSiteBIsAllowed() {
        assertEquals(
            CallRefusal.NONE,
            canStart(peerId = "u-b1", peerStation = "SITE_B", localCell = "SITE_C", selfId = "u-c1")
        )
    }

    @Test
    fun everyOrderedPairOfTheThreeCellsIsAllowed() {
        val cells = listOf("SITE_A", "SITE_B", "SITE_C")
        for (local in cells) {
            for (peer in cells) {
                assertEquals(
                    "call $local -> $peer",
                    CallRefusal.NONE,
                    canStart(peerId = "u-$peer", peerStation = peer, localCell = local)
                )
            }
        }
    }

    @Test
    fun aPeerOnAStationOutsideTheDeploymentIsRefused() {
        assertEquals(
            CallRefusal.UNKNOWN_CELL,
            canStart(peerId = "u-x1", peerStation = "SITE_D", localCell = "SITE_A")
        )
        assertTrue(CallSessionPolicy.refusalMessage(CallRefusal.UNKNOWN_CELL).isNotBlank())
    }

    @Test
    fun theCrossCellFlagMatchesTheCells() {
        assertTrue(CallSessionPolicy.isCrossCell("SITE_A", "SITE_B"))
        assertTrue(CallSessionPolicy.isCrossCell("SITE_C", "SITE_A"))
        assertFalse(CallSessionPolicy.isCrossCell("SITE_A", "SITE_A"))
        assertFalse(CallSessionPolicy.isCrossCell("site_b", "SITE_B"))
        // Without a known local cell, nothing is a cross cell call.
        assertFalse(CallSessionPolicy.isCrossCell(null, "SITE_B"))
        assertFalse(CallSessionPolicy.isCrossCell("SITE_A", null))
    }

    // -----------------------------------------------------------------------
    // Identity, availability, connection
    // -----------------------------------------------------------------------
    @Test
    fun aUserThatIsNotInTheDirectoryCannotBeCalled() {
        assertEquals(CallRefusal.UNKNOWN_USER, canStart(peerId = "u-ghost", peerStation = null))
        assertEquals(CallRefusal.UNKNOWN_USER, canStart(peerId = "u-ghost", peerStation = ""))
        assertEquals(CallRefusal.UNKNOWN_USER, canStart(peerId = "  ", peerStation = "SITE_A"))
    }

    @Test
    fun aUserCannotCallItself() {
        assertEquals(
            CallRefusal.SELF,
            canStart(peerId = "u-a1", peerStation = "SITE_A", localCell = "SITE_A", selfId = "u-a1")
        )
    }

    @Test
    fun aBusyUserCannotStartASecondCall() {
        for (phase in listOf(CallPhase.DIALING, CallPhase.RINGING, CallPhase.ACTIVE)) {
            assertEquals(CallRefusal.BUSY, canStart(phase = phase))
        }
    }

    @Test
    fun withoutACellTheCallIsRefused() {
        assertEquals(CallRefusal.NO_CELL, canStart(localCell = null))
        assertEquals(CallRefusal.NO_CELL, canStart(localCell = ""))
    }

    @Test
    fun withoutAStationTheCallIsRefused() {
        assertEquals(CallRefusal.NOT_CONNECTED, canStart(connected = false))
    }

    // -----------------------------------------------------------------------
    // Phase machine
    // -----------------------------------------------------------------------
    @Test
    fun anOutgoingCallFollowsDialingThenActive() {
        // The station confirms the invitation with call_ringing: the phase stays
        // DIALING (only the label changes), then call_accepted opens the media.
        var phase = CallPhase.DIALING
        phase = CallSessionPolicy.applyEvent(phase, "call_ringing", isCaller = true)
        assertEquals(CallPhase.DIALING, phase)
        // A duplicated ringing event changes nothing either.
        phase = CallSessionPolicy.applyEvent(phase, "call_ringing", isCaller = true)
        assertEquals(CallPhase.DIALING, phase)
        phase = CallSessionPolicy.applyEvent(phase, "call_accepted", isCaller = true)
        assertEquals(CallPhase.ACTIVE, phase)
    }

    @Test
    fun anInvitationNeverMovesAnOutgoingCall() {
        // A cross talk event must not turn an outgoing call into an incoming one.
        assertEquals(
            CallPhase.DIALING,
            CallSessionPolicy.applyEvent(CallPhase.DIALING, "call_invite", isCaller = false)
        )
    }

    @Test
    fun anIncomingCallFollowsRingingThenActive() {
        var phase = CallPhase.IDLE
        phase = CallSessionPolicy.applyEvent(phase, "call_invite", isCaller = false)
        assertEquals(CallPhase.RINGING, phase)
        phase = CallSessionPolicy.applyEvent(phase, "call_accepted", isCaller = false)
        assertEquals(CallPhase.ACTIVE, phase)
    }

    @Test
    fun terminalEventsEndTheCall() {
        for (event in listOf("call_rejected", "call_cancelled", "call_ended", "call_timeout")) {
            assertEquals(
                CallPhase.ENDED,
                CallSessionPolicy.applyEvent(CallPhase.ACTIVE, event, isCaller = true)
            )
            assertEquals(
                CallPhase.ENDED,
                CallSessionPolicy.applyEvent(CallPhase.DIALING, event, isCaller = true)
            )
            assertEquals(
                CallPhase.ENDED,
                CallSessionPolicy.applyEvent(CallPhase.RINGING, event, isCaller = false)
            )
        }
    }

    @Test
    fun anEventOnAnIdleApplicationIsIgnored() {
        assertEquals(
            CallPhase.IDLE,
            CallSessionPolicy.applyEvent(CallPhase.IDLE, "call_accepted", isCaller = false)
        )
        assertEquals(
            CallPhase.IDLE,
            CallSessionPolicy.applyEvent(CallPhase.IDLE, "call_ended", isCaller = true)
        )
        assertEquals(
            CallPhase.IDLE,
            CallSessionPolicy.applyEvent(CallPhase.IDLE, "call_ringing", isCaller = true)
        )
    }

    @Test
    fun anInvitationIsNeverTakenWhileAlreadyBusy() {
        assertEquals(
            CallPhase.ACTIVE,
            CallSessionPolicy.applyEvent(CallPhase.ACTIVE, "call_invite", isCaller = false)
        )
        assertEquals(
            CallPhase.DIALING,
            CallSessionPolicy.applyEvent(CallPhase.DIALING, "call_invite", isCaller = false)
        )
    }

    @Test
    fun anExpiredInvitationIsTerminal() {
        assertEquals(
            CallPhase.ENDED,
            CallSessionPolicy.applyEvent(CallPhase.RINGING, "call_timeout", isCaller = false)
        )
        assertTrue(CallSessionPolicy.isTerminal(CallPhase.ENDED))
        assertFalse(CallSessionPolicy.isTerminal(CallPhase.ACTIVE))
    }

    @Test
    fun onlyTheRelevantActionIsOfferedPerPhase() {
        assertTrue(CallSessionPolicy.canAnswer(CallPhase.RINGING))
        assertFalse(CallSessionPolicy.canAnswer(CallPhase.DIALING))
        assertTrue(CallSessionPolicy.canCancel(CallPhase.DIALING))
        assertFalse(CallSessionPolicy.canCancel(CallPhase.ACTIVE))
        assertTrue(CallSessionPolicy.canHangUp(CallPhase.ACTIVE))
        assertFalse(CallSessionPolicy.canHangUp(CallPhase.DIALING))
    }

    @Test
    fun endReasonsAreHumanReadable() {
        assertEquals("Appel refusé", CallSessionPolicy.endReasonLabel("rejected"))
        assertEquals("Appel annulé", CallSessionPolicy.endReasonLabel("cancelled"))
        assertEquals("Appel terminé", CallSessionPolicy.endReasonLabel("hangup"))
        assertTrue(CallSessionPolicy.endReasonLabel("disconnected").contains("déconnecté"))
        assertTrue(CallSessionPolicy.endReasonLabel(null).isNotBlank())
    }
}
