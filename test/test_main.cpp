// Entry point of the unit test suite.
//
//  * env:native  -> pio test -e native   (host binary, no hardware required)
//  * env:site_*  -> pio test -e site_a   (same tests compiled for the ESP32)
//
// The test functions live in test_protocol.cpp and test_core.cpp.

#include <unity.h>

void testProtocolRoundtrip();
void testProtocolCrcRejectsCorruption();
void testProtocolRejectsTruncatedFrame();
void testProtocolRejectsOversizedPayload();
void testAssemblerResynchronisesAfterGarbage();
void testAssemblerHandlesByteByByteStream();
void testProtocolTypeNames();

void testQueueServesHighestPriorityFirst();
void testQueueIsBoundedAndCountsDrops();
void testQueuePromotesStarvedAudio();
void testArbiterNeedsTokenToTransmit();
void testArbiterHandsOverWhenBudgetExhausted();
void testArbiterClaimsTokenAfterSilence();
void testArbiterSkipsDeadStations();
void testArbiterCountsIdleRounds();
void testArbiterRotatesOverAliveNodesAndExposesBudget();
void testRoutingUpsertMovesUserBetweenStations();
void testRoutingExpiresSilentLocalUsers();
void testRoutingPresenceStateRoundtrip();
void testRoutingTruncatedPresenceStateIsRejected();
void testRoutingPartialPresenceStateIsRejectedAtomically();
void testRoutingMarksWholeStationOffline();
void testEnvelopeRoundtrip();
void testEnvelopeRejectsBadLengths();
void testFragmentAssemblyOutOfOrder();
void testFragmentDuplicateIsIgnored();
void testFragmentRejectsInvalidIndex();
void testFragmentContextExpires();

// test_call.cpp : real time calls (local session table and cross cell sessions
// over the Wi-Fi backbone) plus the structural guards proving that call audio
// never travels on the RS485 bus.
void testCallInviteWithinSameCellIsAccepted();
void testCallAcceptThenHangupCompletesTheCall();
void testCallToARecipientOfAnotherCellUsesTheBackbone();
void testRemoteInvitationIsAcceptedAndReportedAsRemote();
void testRemoteCallBecomesActiveOnlyWhenThePeerAccepts();
void testIncomingRemoteInvitationRingsTheLocalCallee();
void testIncomingRemoteInvitationIsRefusedWhenTheCalleeIsAbsent();
void testRemoteEntriesRejectContradictoryNodes();
void testRemoteEndReleasesTheSession();
void testRemoteInvitationExpiresWithItsRemoteNode();
void testRemoteDisconnectionReportsTheRemoteNode();
void testSimultaneousCrossCellCallsAreRefusedWhenAUserIsBusy();
void testCallFromAUserThatIsNotConnectedHereIsRefused();
void testALocalCallIsStillPossibleWhileTheBackboneIsDown();
void testCallFromAUserOfAnotherCellIsRefused();
void testCallToAnUnknownUserIsRefused();
void testCallWithoutAProbeIsRefused();
void testCallToSelfIsRefused();
void testBusyUserCannotReceiveASecondIncompatibleSession();
void testCallerCannotAcceptItsOwnInvitation();
void testAcceptRequiresARingingSession();
void testExpiredInvitationCannotBeAccepted();
void testRingingSessionExpiresOnTheStationSide();
void testDisconnectionReleasesTheSessionAndThePeerIsReported();
void testEndForUserWithoutSessionIsReported();
void testEstablishedCallWithoutMediaIsDropped();
void testMediaKeepsTheSessionAlive();
void testRepeatedInvitationsAreRateLimited();
void testInvalidCallIdIsRefused();
void testCallTableIsBoundedAndNeverLeaks();
void testMediaRelayOnlyInsideAnActiveSession();
void testMediaRelayStopsWhenTheCallEnds();
void testCallAudioFrameRoundTrip();
void testCallAudioFrameRejectsMalformedInput();
void testCallAudioFrameBuilderRejectsInvalidInput();
void testCallAudioMagicCannotBeMistakenForABusFrame();
void testCallModuleDoesNotDependOnTheBusLibraries();
void testStationCallHandlersNeverSubmitToTheMesh();
void testMeshNodeAndProtocolWereNotTouchedByTheCallFeature();

// test_link.cpp : inter-station Wi-Fi backbone (framing, authentication, hop
// limited routing, reconnection, liveness) and its structural guards.
void testLinkFrameRoundTrip();
void testLinkFrameRejectsMalformedInput();
void testLinkStreamAssemblerRebuildsFramesFromAStream();
void testLinkStreamAssemblerResynchronisesAfterGarbage();
void testLinkHelloRoundTripAndTokenCheck();
void testLinkTokenComparisonIsExact();
void testLinkCallSignalRoundTrip();
void testLinkCallSignalRejectsMalformedInput();
void testLinkRoutesStarTopologyServesEveryCellPair();
void testLinkRoutesChainTopologyConvergesThroughAnnouncements();
void testLinkRoutesRelayNeverLoopsBack();
void testLinkRoutesHopLimitDropsTheFrame();
void testLinkRoutesForgetTheLearnedPathAfterALinkLoss();
void testLinkClientStateReconnectsWithDelay();
void testLinkLivenessDetectsASilentPeer();
void testLinkModuleDoesNotDependOnTheBusLibraries();
void testStationLinkGlueNeverUsesTheMesh();
void testWifiBackboneGlueNeverUsesTheMesh();
void testTheBusProtocolAndTheMeshWereNotTouchedByTheBackbone();
void testTheBackboneFrameMagicCannotBeMistakenForABusFrame();
void testTheRecordedVoicePathIsUntouchedByTheBackbone();

// test_reliability.cpp : acknowledgements, protected queueing, long messages and
// the deterministic two sender simulation.
void testAckTrackerClearsOnlyTheAcknowledgedFragment();
void testAckTrackerRetransmitsOnlyTheMissingFragment();
void testAckTrackerPostponesWhileThePeerIsAlive();
void testAckTrackerFailsOnlyAfterAllAttempts();
void testAckTrackerRejectsWhenFullAndCountsIt();
void testControlQueueNeverTakesThePlaceOfUserMessages();
void testControlQueueIsBoundedAndReported();
void testFragmentAssemblyTwelveFragmentsOutOfOrder();
void testFragmentAssemblyKeepsTwoConcurrentMessagesApart();
void testFragmentAssemblyExposesTheFullPayloadWithoutCopy();
void testFragmentAssemblyClearsTheViewOnIncompleteAndRejectedInput();
void testFragmentAssemblyRejectsOutputBufferTooSmall();
void testConcurrentSendersBothMessagesArriveExactlyOnce();
void testConcurrentSendersAreFairInBothDirections();
void testConcurrentSendersRecoverFromALostToken();
void testSilentPeerIsRecoveredByRetransmission();

// Required by the Unity runner (called before/after every test).
extern "C" void setUp(void) {}
extern "C" void tearDown(void) {}

static int runAllTests() {
  UNITY_BEGIN();
  RUN_TEST(testProtocolRoundtrip);
  RUN_TEST(testProtocolCrcRejectsCorruption);
  RUN_TEST(testProtocolRejectsTruncatedFrame);
  RUN_TEST(testProtocolRejectsOversizedPayload);
  RUN_TEST(testAssemblerResynchronisesAfterGarbage);
  RUN_TEST(testAssemblerHandlesByteByByteStream);
  RUN_TEST(testProtocolTypeNames);
  RUN_TEST(testCallInviteWithinSameCellIsAccepted);
  RUN_TEST(testCallAcceptThenHangupCompletesTheCall);
  RUN_TEST(testCallToARecipientOfAnotherCellUsesTheBackbone);
  RUN_TEST(testRemoteInvitationIsAcceptedAndReportedAsRemote);
  RUN_TEST(testRemoteCallBecomesActiveOnlyWhenThePeerAccepts);
  RUN_TEST(testIncomingRemoteInvitationRingsTheLocalCallee);
  RUN_TEST(testIncomingRemoteInvitationIsRefusedWhenTheCalleeIsAbsent);
  RUN_TEST(testRemoteEntriesRejectContradictoryNodes);
  RUN_TEST(testRemoteEndReleasesTheSession);
  RUN_TEST(testRemoteInvitationExpiresWithItsRemoteNode);
  RUN_TEST(testRemoteDisconnectionReportsTheRemoteNode);
  RUN_TEST(testSimultaneousCrossCellCallsAreRefusedWhenAUserIsBusy);
  RUN_TEST(testALocalCallIsStillPossibleWhileTheBackboneIsDown);
  RUN_TEST(testCallFromAUserThatIsNotConnectedHereIsRefused);
  RUN_TEST(testCallToAnUnknownUserIsRefused);
  RUN_TEST(testCallWithoutAProbeIsRefused);
  RUN_TEST(testCallToSelfIsRefused);
  RUN_TEST(testBusyUserCannotReceiveASecondIncompatibleSession);
  RUN_TEST(testCallerCannotAcceptItsOwnInvitation);
  RUN_TEST(testAcceptRequiresARingingSession);
  RUN_TEST(testExpiredInvitationCannotBeAccepted);
  RUN_TEST(testRingingSessionExpiresOnTheStationSide);
  RUN_TEST(testDisconnectionReleasesTheSessionAndThePeerIsReported);
  RUN_TEST(testEndForUserWithoutSessionIsReported);
  RUN_TEST(testEstablishedCallWithoutMediaIsDropped);
  RUN_TEST(testMediaKeepsTheSessionAlive);
  RUN_TEST(testRepeatedInvitationsAreRateLimited);
  RUN_TEST(testInvalidCallIdIsRefused);
  RUN_TEST(testCallTableIsBoundedAndNeverLeaks);
  RUN_TEST(testMediaRelayOnlyInsideAnActiveSession);
  RUN_TEST(testMediaRelayStopsWhenTheCallEnds);
  RUN_TEST(testCallAudioFrameRoundTrip);
  RUN_TEST(testCallAudioFrameRejectsMalformedInput);
  RUN_TEST(testCallAudioFrameBuilderRejectsInvalidInput);
  RUN_TEST(testCallAudioMagicCannotBeMistakenForABusFrame);
  RUN_TEST(testCallModuleDoesNotDependOnTheBusLibraries);
  RUN_TEST(testStationCallHandlersNeverSubmitToTheMesh);
  RUN_TEST(testMeshNodeAndProtocolWereNotTouchedByTheCallFeature);
  RUN_TEST(testLinkFrameRoundTrip);
  RUN_TEST(testLinkFrameRejectsMalformedInput);
  RUN_TEST(testLinkStreamAssemblerRebuildsFramesFromAStream);
  RUN_TEST(testLinkStreamAssemblerResynchronisesAfterGarbage);
  RUN_TEST(testLinkHelloRoundTripAndTokenCheck);
  RUN_TEST(testLinkTokenComparisonIsExact);
  RUN_TEST(testLinkCallSignalRoundTrip);
  RUN_TEST(testLinkCallSignalRejectsMalformedInput);
  RUN_TEST(testLinkRoutesStarTopologyServesEveryCellPair);
  RUN_TEST(testLinkRoutesChainTopologyConvergesThroughAnnouncements);
  RUN_TEST(testLinkRoutesRelayNeverLoopsBack);
  RUN_TEST(testLinkRoutesHopLimitDropsTheFrame);
  RUN_TEST(testLinkRoutesForgetTheLearnedPathAfterALinkLoss);
  RUN_TEST(testLinkClientStateReconnectsWithDelay);
  RUN_TEST(testLinkLivenessDetectsASilentPeer);
  RUN_TEST(testLinkModuleDoesNotDependOnTheBusLibraries);
  RUN_TEST(testStationLinkGlueNeverUsesTheMesh);
  RUN_TEST(testWifiBackboneGlueNeverUsesTheMesh);
  RUN_TEST(testTheBusProtocolAndTheMeshWereNotTouchedByTheBackbone);
  RUN_TEST(testTheBackboneFrameMagicCannotBeMistakenForABusFrame);
  RUN_TEST(testTheRecordedVoicePathIsUntouchedByTheBackbone);
  RUN_TEST(testQueueServesHighestPriorityFirst);
  RUN_TEST(testQueueIsBoundedAndCountsDrops);
  RUN_TEST(testQueuePromotesStarvedAudio);
  RUN_TEST(testArbiterNeedsTokenToTransmit);
  RUN_TEST(testArbiterHandsOverWhenBudgetExhausted);
  RUN_TEST(testArbiterClaimsTokenAfterSilence);
  RUN_TEST(testArbiterSkipsDeadStations);
  RUN_TEST(testArbiterCountsIdleRounds);
  RUN_TEST(testArbiterRotatesOverAliveNodesAndExposesBudget);
  RUN_TEST(testRoutingUpsertMovesUserBetweenStations);
  RUN_TEST(testRoutingExpiresSilentLocalUsers);
  RUN_TEST(testRoutingPresenceStateRoundtrip);
  RUN_TEST(testRoutingTruncatedPresenceStateIsRejected);
  RUN_TEST(testRoutingPartialPresenceStateIsRejectedAtomically);
  RUN_TEST(testRoutingMarksWholeStationOffline);
  RUN_TEST(testEnvelopeRoundtrip);
  RUN_TEST(testEnvelopeRejectsBadLengths);
  RUN_TEST(testFragmentAssemblyOutOfOrder);
  RUN_TEST(testFragmentDuplicateIsIgnored);
  RUN_TEST(testFragmentRejectsInvalidIndex);
  RUN_TEST(testFragmentContextExpires);
  RUN_TEST(testAckTrackerClearsOnlyTheAcknowledgedFragment);
  RUN_TEST(testAckTrackerRetransmitsOnlyTheMissingFragment);
  RUN_TEST(testAckTrackerPostponesWhileThePeerIsAlive);
  RUN_TEST(testAckTrackerFailsOnlyAfterAllAttempts);
  RUN_TEST(testAckTrackerRejectsWhenFullAndCountsIt);
  RUN_TEST(testControlQueueNeverTakesThePlaceOfUserMessages);
  RUN_TEST(testControlQueueIsBoundedAndReported);
  RUN_TEST(testFragmentAssemblyTwelveFragmentsOutOfOrder);
  RUN_TEST(testFragmentAssemblyKeepsTwoConcurrentMessagesApart);
  RUN_TEST(testFragmentAssemblyExposesTheFullPayloadWithoutCopy);
  RUN_TEST(testFragmentAssemblyClearsTheViewOnIncompleteAndRejectedInput);
  RUN_TEST(testFragmentAssemblyRejectsOutputBufferTooSmall);
  RUN_TEST(testConcurrentSendersBothMessagesArriveExactlyOnce);
  RUN_TEST(testConcurrentSendersAreFairInBothDirections);
  RUN_TEST(testConcurrentSendersRecoverFromALostToken);
  RUN_TEST(testSilentPeerIsRecoveredByRetransmission);
  return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>

void setup() {
  delay(200);
  runAllTests();
}

void loop() {
  // nothing to do
}
#else
int main(int, char**) {
  return runAllTests();
}
#endif
