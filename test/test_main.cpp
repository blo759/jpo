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

// test_call.cpp : local real time calls (session table, cellular isolation,
// timeouts, media relay validation) and the structural guards proving that call
// audio never travels on the RS485 bus.
void testCallInviteWithinSameCellIsAccepted();
void testCallAcceptThenHangupCompletesTheCall();
void testCallBetweenTwoCellsIsRefused();
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
  RUN_TEST(testCallBetweenTwoCellsIsRefused);
  RUN_TEST(testCallFromAUserOfAnotherCellIsRefused);
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
