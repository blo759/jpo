// Tests of the local real time call feature (lib/cell_call).
//
// Two families of checks:
//   * behaviour of the CallTable (cellular isolation, busy, timeout,
//     disconnection, media relay validation) and of the media frame codec,
//   * structural guards proving that the call feature is *not* wired into the
//     RS485 transport. Those guards read the real source files, so a later
//     change that would route call audio through the bus makes the suite fail.

#include <unity.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "cell_call.h"
#include "cell_protocol.h"

namespace {

// ---------------------------------------------------------------------------
// Fake station membership: exactly what the ESP32 WebSocket binding table does.
// Users of "other cells" simply do not appear here, which is how a cross cell
// call is refused.
// ---------------------------------------------------------------------------
struct FakeCell {
  const char* users[8] = {nullptr};
  size_t count = 0;
};

void addUser(FakeCell& cell, const char* userId) {
  cell.users[cell.count++] = userId;
}

bool probeFake(void* context, const char* userId) {
  FakeCell* cell = static_cast<FakeCell*>(context);
  if (cell == nullptr || userId == nullptr) {
    return false;
  }
  for (size_t i = 0; i < cell->count; ++i) {
    if (strcmp(cell->users[i], userId) == 0) {
      return true;
    }
  }
  return false;
}

constexpr const char* kCallA = "call0001";
constexpr const char* kCallB = "call0002";
constexpr const char* kCallC = "call0003";
constexpr const char* kCallD = "call0004";

std::string readSource(const char* relativePath) {
  const char* prefixes[] = {"", "../", "../../", "../../../"};
  for (const char* prefix : prefixes) {
    std::ifstream stream(std::string(prefix) + relativePath);
    if (!stream.good()) {
      continue;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  }
  return std::string();
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// Returns the text between two markers, or an empty string when the markers are
// missing (which the calling test reports as a failure).
std::string between(const std::string& text, const char* start, const char* end) {
  const size_t from = text.find(start);
  if (from == std::string::npos) {
    return std::string();
  }
  const size_t to = text.find(end, from);
  if (to == std::string::npos) {
    return std::string();
  }
  return text.substr(from, to - from);
}

}  // namespace

// ---------------------------------------------------------------------------
// CallTable: allowed calls, one cell at a time
// ---------------------------------------------------------------------------
void testCallInviteWithinSameCellIsAccepted() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  const cell::CallSession* session = table.find(kCallA);
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallState::kRinging),
                    static_cast<int>(session->state));
  TEST_ASSERT_EQUAL_STRING("u-a1", session->caller);
  TEST_ASSERT_EQUAL_STRING("u-a2", session->callee);
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(table.activeSessions()));
  TEST_ASSERT_TRUE(table.isBusy("u-a1"));
  TEST_ASSERT_TRUE(table.isBusy("u-a2"));
}

void testCallAcceptThenHangupCompletesTheCall() {
  FakeCell cellB;
  addUser(cellB, "u-b1");
  addUser(cellB, "u-b2");
  cell::CallTable table;
  table.begin(probeFake, &cellB, 2);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallB, "u-b1", "u-b2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallB, "u-b2", 1500U)));
  const cell::CallSession* session = table.find(kCallB);
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallState::kActive),
                    static_cast<int>(session->state));
  TEST_ASSERT_EQUAL_UINT32(1500U, session->answeredAtMs);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.hangup(kCallB, "u-b1", 9000U)));
  TEST_ASSERT_NULL(table.find(kCallB));
  TEST_ASSERT_FALSE(table.isBusy("u-b1"));
  TEST_ASSERT_EQUAL_UINT32(1U, table.completedCalls());
}

// ---------------------------------------------------------------------------
// Cellular routing of a call
// ---------------------------------------------------------------------------
void testCallToARecipientOfAnotherCellUsesTheBackbone() {
  // The local call path (invite) is unchanged: it requires both participants to
  // hold a socket on this station. A cross cell call goes through the remote
  // entries of the table instead, which is what the station uses when the
  // routing table places the recipient on another node.
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  const cell::CallStatus status = table.invite(kCallA, "u-a1", "u-b1", 1000U);
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotLocal), static_cast<int>(status));
  TEST_ASSERT_EQUAL_STRING("not-local", cell::callStatusName(status));
  TEST_ASSERT_NULL(table.find(kCallA));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.activeSessions()));
  TEST_ASSERT_EQUAL_UINT32(1U, table.refusedCalls());
  TEST_ASSERT_EQUAL_STRING("not-local", table.lastRefusal());
}

void testRemoteInvitationIsAcceptedAndReportedAsRemote() {
  // A1 (SITE_A) calls B1 (SITE_B): the session is created locally for A1 and
  // knows that the other participant lives on node 2.
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-b1", 2U, 1000U)));
  const cell::CallSession* session = table.find(kCallA);
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL_UINT8(2U, session->remoteNode);
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallState::kRinging),
                    static_cast<int>(session->state));
  // The remote station has not confirmed anything yet: the call must not be
  // reported as ringing on the other side before that confirmation.
  TEST_ASSERT_FALSE(session->remoteConfirmed);
  TEST_ASSERT_TRUE(table.isRemote(kCallA));
  TEST_ASSERT_EQUAL_UINT8(2U, table.remoteNodeOf(kCallA));
  TEST_ASSERT_TRUE(table.isBusy("u-a1"));

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.noteRemoteRinging(kCallA, 1200U)));
  TEST_ASSERT_TRUE(table.find(kCallA)->remoteConfirmed);
}

void testRemoteCallBecomesActiveOnlyWhenThePeerAccepts() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-c1", 3U, 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.noteRemoteRinging(kCallA, 1100U)));
  // No media is relayed before the remote acceptance.
  TEST_ASSERT_NULL(table.mediaPeer(kCallA, "u-a1"));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.acceptRemote(kCallA, 1500U)));
  const cell::CallSession* session = table.find(kCallA);
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallState::kActive),
                    static_cast<int>(session->state));
  TEST_ASSERT_EQUAL_UINT32(1500U, session->answeredAtMs);
  // Both directions are valid: the local participant and the remote one.
  TEST_ASSERT_EQUAL_STRING("u-c1", table.mediaPeer(kCallA, "u-a1"));
  TEST_ASSERT_EQUAL_STRING("u-a1", table.mediaPeer(kCallA, "u-c1"));
  // A third party is never a participant of a cross cell session.
  TEST_ASSERT_NULL(table.mediaPeer(kCallA, "u-c2"));
}

void testIncomingRemoteInvitationRingsTheLocalCallee() {
  // SITE_B receives the invitation of A1 for B1: the caller is remote, the
  // callee is here, and the local accept path is the unchanged one.
  FakeCell cellB;
  addUser(cellB, "u-b1");
  cell::CallTable table;
  table.begin(probeFake, &cellB, 2);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteIncoming(kCallA, "u-a1", "u-b1", 1U, 1000U)));
  const cell::CallSession* session = table.find(kCallA);
  TEST_ASSERT_NOT_NULL(session);
  TEST_ASSERT_EQUAL_UINT8(1U, session->remoteNode);
  TEST_ASSERT_EQUAL_STRING("u-a1", session->caller);
  TEST_ASSERT_EQUAL_STRING("u-b1", session->callee);
  TEST_ASSERT_TRUE(session->remoteConfirmed);
  TEST_ASSERT_TRUE(table.isBusy("u-b1"));
  // The remote caller is never probed here: only the local callee must exist.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-b1", 1500U)));
  TEST_ASSERT_EQUAL_STRING("u-a1", table.mediaPeer(kCallA, "u-b1"));
}

void testIncomingRemoteInvitationIsRefusedWhenTheCalleeIsAbsent() {
  FakeCell cellB;
  addUser(cellB, "u-b1");
  cell::CallTable table;
  table.begin(probeFake, &cellB, 2);

  // u-b2 is not connected to this station: the invitation cannot be delivered.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotLocal),
                    static_cast<int>(table.inviteIncoming(kCallB, "u-a1", "u-b2", 1U, 1000U)));
  TEST_ASSERT_NULL(table.find(kCallB));
}

void testRemoteEntriesRejectContradictoryNodes() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  // "remote on my own station" is a contradiction.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidState),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-a2", 1U, 1000U)));
  // An incoming invitation without a remote station is meaningless.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidState),
                    static_cast<int>(table.inviteIncoming(kCallA, "u-a1", "u-a2", 0U, 1000U)));
  // A node outside the deployment is refused too.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidState),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-a2", 9U, 1000U)));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.activeSessions()));
}

void testRemoteEndReleasesTheSession() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-b1", 2U, 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.acceptRemote(kCallA, 1500U)));
  cell::CallSession ended;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.remoteEnd(kCallA, 5000U, ended)));
  TEST_ASSERT_NULL(table.find(kCallA));
  TEST_ASSERT_FALSE(table.isBusy("u-a1"));
  TEST_ASSERT_EQUAL_UINT32(1U, table.completedCalls());
  // The copy handed back to the caller must still describe the call that was
  // released: this is what lets the station notify the local participant AFTER
  // the release. Reading the session through a pointer into the table returned an
  // empty user id, so the notification was silently dropped and the remote side
  // kept ringing for ever.
  TEST_ASSERT_EQUAL_STRING(kCallA, ended.callId);
  TEST_ASSERT_EQUAL_STRING("u-a1", ended.caller);
  TEST_ASSERT_EQUAL_STRING("u-b1", ended.callee);
  TEST_ASSERT_EQUAL_UINT8(2U, ended.remoteNode);
  // Ending a cross cell call that is already gone is not an error: the other
  // side may have released it first, and the copy must be cleared.
  cell::CallSession again;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.remoteEnd(kCallA, 6000U, again)));
  TEST_ASSERT_EQUAL_STRING("", again.callId);
  TEST_ASSERT_EQUAL_STRING("", again.caller);
  TEST_ASSERT_EQUAL_STRING("", again.callee);
}

void testRemoteInvitationExpiresWithItsRemoteNode() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-b1", 2U, 1000U)));
  cell::CallSession expired;
  TEST_ASSERT_FALSE(table.expireRinging(1000U + cell::kCallRingTimeoutMs, expired));
  TEST_ASSERT_TRUE(table.expireRinging(1000U + cell::kCallRingTimeoutMs + 1U, expired));
  TEST_ASSERT_EQUAL_STRING(kCallA, expired.callId);
  // The station needs the remote node to tell the other cell to stop ringing.
  TEST_ASSERT_EQUAL_UINT8(2U, expired.remoteNode);
  TEST_ASSERT_NULL(table.find(kCallA));
}

void testRemoteDisconnectionReportsTheRemoteNode() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-b1", 2U, 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.acceptRemote(kCallA, 1500U)));

  cell::CallSession ended;
  TEST_ASSERT_EQUAL(
      static_cast<int>(cell::CallStatus::kOk),
      static_cast<int>(table.endForUser("u-a1", cell::CallEndReason::kDisconnected, 4000U,
                                        ended)));
  TEST_ASSERT_EQUAL_STRING(kCallA, ended.callId);
  TEST_ASSERT_EQUAL_STRING("u-b1", ended.callee);
  TEST_ASSERT_EQUAL_UINT8(2U, ended.remoteNode);
  TEST_ASSERT_NULL(table.find(kCallA));
}

void testSimultaneousCrossCellCallsAreRefusedWhenAUserIsBusy() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.inviteRemote(kCallA, "u-a1", "u-b1", 2U, 1000U)));
  // The same local user cannot start a second cross cell call.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kBusy),
                    static_cast<int>(table.inviteRemote(kCallB, "u-a1", "u-c1", 3U, 5000U)));
  // Nor accept an incoming one while already busy.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kBusy),
                    static_cast<int>(table.inviteIncoming(kCallC, "u-b1", "u-a1", 2U, 5000U)));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(table.activeSessions()));
}

void testALocalCallIsStillPossibleWhileTheBackboneIsDown() {
  // The local path never consults the backbone: a same-cell call works even when
  // the inter-station link is unusable.
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1200U)));
  TEST_ASSERT_FALSE(table.isRemote(kCallA));
  TEST_ASSERT_EQUAL_UINT8(0U, table.remoteNodeOf(kCallA));
  TEST_ASSERT_EQUAL_STRING("u-a2", table.mediaPeer(kCallA, "u-a1"));
  TEST_ASSERT_EQUAL_STRING("u-a1", table.mediaPeer(kCallA, "u-a2"));
}

void testCallFromAUserThatIsNotConnectedHereIsRefused() {
  // Even a locally known callee cannot be called by someone who is not connected
  // to this station: the caller is probed too.
  FakeCell cellC;
  addUser(cellC, "u-c1");
  cell::CallTable table;
  table.begin(probeFake, &cellC, 3);

  const cell::CallStatus status = table.invite(kCallC, "u-b1", "u-c1", 1000U);
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotLocal), static_cast<int>(status));
  TEST_ASSERT_NULL(table.find(kCallC));
  // A cross cell call is refused as well when the caller is not here.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotLocal),
                    static_cast<int>(table.inviteRemote(kCallC, "u-b1", "u-a1", 1U, 4000U)));
}

void testCallToAnUnknownUserIsRefused() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotLocal),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-ghost", 1000U)));
  TEST_ASSERT_NULL(table.find(kCallA));
}

void testCallWithoutAProbeIsRefused() {
  // A station that did not install its membership probe can never place a call.
  cell::CallTable table;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotLocal),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
}

void testCallToSelfIsRefused() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kSelfCall),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a1", 1000U)));
  TEST_ASSERT_NULL(table.find(kCallA));
}

// ---------------------------------------------------------------------------
// Busy, invalid state, participants
// ---------------------------------------------------------------------------
void testBusyUserCannotReceiveASecondIncompatibleSession() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  addUser(cellA, "u-a3");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  // u-a2 is already ringing: a third user cannot grab it.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kBusy),
                    static_cast<int>(table.invite(kCallB, "u-a3", "u-a2", 4000U)));
  // The caller is busy as well.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kBusy),
                    static_cast<int>(table.invite(kCallC, "u-a1", "u-a3", 4000U)));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(table.activeSessions()));
  TEST_ASSERT_NULL(table.find(kCallB));
  TEST_ASSERT_NULL(table.find(kCallC));
}

void testCallerCannotAcceptItsOwnInvitation() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotParticipant),
                    static_cast<int>(table.accept(kCallA, "u-a1", 1200U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kNotParticipant),
                    static_cast<int>(table.hangup(kCallA, "u-a3", 1200U)));
  // The session is untouched by a rejected transition.
  TEST_ASSERT_NOT_NULL(table.find(kCallA));
}

void testAcceptRequiresARingingSession() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kUnknownCall),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1500U)));
  // Accepting twice is an invalid state, not a second session.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidState),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1600U)));
}

// ---------------------------------------------------------------------------
// Timeouts and disconnections
// ---------------------------------------------------------------------------
void testExpiredInvitationCannotBeAccepted() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  const uint32_t late = 1000U + cell::kCallRingTimeoutMs + 1U;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kExpired),
                    static_cast<int>(table.accept(kCallA, "u-a2", late)));
  TEST_ASSERT_NULL(table.find(kCallA));
  TEST_ASSERT_FALSE(table.isBusy("u-a2"));
}

void testRingingSessionExpiresOnTheStationSide() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  cell::CallSession expired;
  TEST_ASSERT_FALSE(table.expireRinging(1000U + cell::kCallRingTimeoutMs, expired));
  TEST_ASSERT_TRUE(table.expireRinging(1000U + cell::kCallRingTimeoutMs + 1U, expired));
  TEST_ASSERT_EQUAL_STRING(kCallA, expired.callId);
  TEST_ASSERT_NULL(table.find(kCallA));
}

void testDisconnectionReleasesTheSessionAndThePeerIsReported() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  addUser(cellA, "u-a3");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1200U)));

  cell::CallSession ended;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.endForUser("u-a2", cell::CallEndReason::kDisconnected,
                                                      5000U, ended)));
  TEST_ASSERT_EQUAL_STRING("u-a1", ended.caller);
  TEST_ASSERT_EQUAL_STRING(kCallA, ended.callId);
  TEST_ASSERT_EQUAL_UINT8(0U, ended.remoteNode);
  TEST_ASSERT_NULL(table.find(kCallA));

  // Both users are free again, and the freed slot is reusable.
  TEST_ASSERT_FALSE(table.isBusy("u-a1"));
  TEST_ASSERT_FALSE(table.isBusy("u-a2"));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallD, "u-a2", "u-a3", 9000U)));
  TEST_ASSERT_NOT_NULL(table.find(kCallD));
}

void testEndForUserWithoutSessionIsReported() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  cell::CallSession ended;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kUnknownCall),
                    static_cast<int>(table.endForUser("u-a1", cell::CallEndReason::kDisconnected,
                                                      1000U, ended)));
  TEST_ASSERT_EQUAL_STRING("", ended.callId);
}

void testEstablishedCallWithoutMediaIsDropped() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1200U)));

  cell::CallSession expired;
  const uint32_t alive = 1200U + cell::kCallMediaTimeoutMs;
  TEST_ASSERT_FALSE(table.expireStaleMedia(alive, expired));
  TEST_ASSERT_TRUE(table.expireStaleMedia(alive + 1U, expired));
  TEST_ASSERT_EQUAL_STRING(kCallA, expired.callId);
  TEST_ASSERT_EQUAL_STRING("u-a1", expired.caller);
  TEST_ASSERT_EQUAL_STRING("u-a2", expired.callee);
  TEST_ASSERT_NULL(table.find(kCallA));
}

void testMediaKeepsTheSessionAlive() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1200U)));

  uint32_t now = 1200U;
  cell::CallSession expired;
  for (int i = 0; i < 40; ++i) {
    now += cell::kCallMediaTimeoutMs / 2U;
    table.noteMedia(kCallA, now);
    TEST_ASSERT_FALSE(table.expireStaleMedia(now, expired));
  }
  TEST_ASSERT_NOT_NULL(table.find(kCallA));
}

// ---------------------------------------------------------------------------
// Anti abuse and capacity
// ---------------------------------------------------------------------------
void testRepeatedInvitationsAreRateLimited() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  addUser(cellA, "u-a3");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.cancel(kCallA, "u-a1", 1000U)));
  // Same caller, immediately again: refused.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kTooSoon),
                    static_cast<int>(table.invite(kCallB, "u-a1", "u-a3", 1100U)));
  // After the cooldown it is accepted again.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallC, "u-a1", "u-a3",
                                                  1000U + cell::kCallInviteCooldownMs)));
}

void testInvalidCallIdIsRefused() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidCallId),
                    static_cast<int>(table.invite("short", "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidCallId),
                    static_cast<int>(table.invite("call0001x", "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kInvalidCallId),
                    static_cast<int>(table.invite("call 001", "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.activeSessions()));
}

void testCallTableIsBoundedAndNeverLeaks() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  const char* callIds[cell::kMaxCallSessions] = {kCallA, kCallB, kCallC, kCallD};
  uint32_t now = 1000U;
  for (size_t i = 0; i < cell::kMaxCallSessions; ++i) {
    TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                      static_cast<int>(table.invite(callIds[i], "u-a1", "u-a2", now)));
    TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                      static_cast<int>(table.accept(callIds[i], "u-a2", now)));
    TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                      static_cast<int>(table.hangup(callIds[i], "u-a1", now)));
    now += cell::kCallInviteCooldownMs;
  }
  // Every slot is reusable: the table is bounded but never leaks.
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.activeSessions()));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cell::kMaxCallSessions),
                           table.completedCalls());
}

// ---------------------------------------------------------------------------
// Media relay validation
// ---------------------------------------------------------------------------
void testMediaRelayOnlyInsideAnActiveSession() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  addUser(cellA, "u-a3");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  // Ringing is not enough: no audio is relayed before the callee accepted.
  TEST_ASSERT_NULL(table.mediaPeer(kCallA, "u-a1"));

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1200U)));
  // Both directions are valid, and only between the two participants.
  TEST_ASSERT_EQUAL_STRING("u-a2", table.mediaPeer(kCallA, "u-a1"));
  TEST_ASSERT_EQUAL_STRING("u-a1", table.mediaPeer(kCallA, "u-a2"));
  TEST_ASSERT_NULL(table.mediaPeer(kCallA, "u-a3"));
  TEST_ASSERT_NULL(table.mediaPeer(kCallB, "u-a1"));
  TEST_ASSERT_NULL(table.mediaPeer(kCallA, nullptr));
}

void testMediaRelayStopsWhenTheCallEnds() {
  FakeCell cellA;
  addUser(cellA, "u-a1");
  addUser(cellA, "u-a2");
  cell::CallTable table;
  table.begin(probeFake, &cellA, 1);

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.invite(kCallA, "u-a1", "u-a2", 1000U)));
  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.accept(kCallA, "u-a2", 1200U)));
  TEST_ASSERT_EQUAL_STRING("u-a2", table.mediaPeer(kCallA, "u-a1"));

  TEST_ASSERT_EQUAL(static_cast<int>(cell::CallStatus::kOk),
                    static_cast<int>(table.hangup(kCallA, "u-a2", 4000U)));
  TEST_ASSERT_NULL(table.mediaPeer(kCallA, "u-a1"));
}

// ---------------------------------------------------------------------------
// Media frame codec
// ---------------------------------------------------------------------------
void testCallAudioFrameRoundTrip() {
  uint8_t pcm[cell::kCallAudioFrameBytes];
  for (size_t i = 0; i < sizeof(pcm); ++i) {
    pcm[i] = static_cast<uint8_t>(i & 0xFFU);
  }
  uint8_t frame[cell::kCallAudioHeaderSize + cell::kCallAudioFrameBytes] = {0};
  const size_t written = cell::buildCallAudioFrame(kCallA, 4242U, pcm, sizeof(pcm), frame,
                                                   sizeof(frame));
  TEST_ASSERT_EQUAL_UINT32(
      static_cast<uint32_t>(cell::kCallAudioHeaderSize + sizeof(pcm)),
      static_cast<uint32_t>(written));
  TEST_ASSERT_EQUAL_UINT8(cell::kCallAudioMagic, frame[0]);
  TEST_ASSERT_EQUAL_UINT8(cell::kCallAudioVersion, frame[1]);

  char callId[cell::kCallIdLength + 1] = {0};
  uint16_t sequence = 0;
  const uint8_t* payload = nullptr;
  size_t payloadLength = 0;
  TEST_ASSERT_TRUE(cell::parseCallAudioFrame(frame, written, callId, sizeof(callId), sequence,
                                             &payload, payloadLength));
  TEST_ASSERT_EQUAL_STRING(kCallA, callId);
  TEST_ASSERT_EQUAL_UINT16(4242U, sequence);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(sizeof(pcm)),
                           static_cast<uint32_t>(payloadLength));
  TEST_ASSERT_NOT_NULL(payload);
  TEST_ASSERT_EQUAL_INT(0, memcmp(payload, pcm, sizeof(pcm)));
}

void testCallAudioFrameRejectsMalformedInput() {
  uint8_t pcm[64];
  memset(pcm, 0x11, sizeof(pcm));
  uint8_t frame[cell::kCallAudioHeaderSize + sizeof(pcm)] = {0};
  const size_t written =
      cell::buildCallAudioFrame(kCallA, 7U, pcm, sizeof(pcm), frame, sizeof(frame));
  TEST_ASSERT_TRUE(written > 0U);

  char callId[cell::kCallIdLength + 1] = {0};
  uint16_t sequence = 0;
  const uint8_t* payload = nullptr;
  size_t payloadLength = 0;
  uint8_t broken[sizeof(frame)];

  // Wrong magic: this is a recorded voice note (0xA5), not a call frame.
  memcpy(broken, frame, written);
  broken[0] = 0xA5U;
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(broken, written, callId, sizeof(callId), sequence,
                                              &payload, payloadLength));

  // Wrong version.
  memcpy(broken, frame, written);
  broken[1] = 0x02U;
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(broken, written, callId, sizeof(callId), sequence,
                                              &payload, payloadLength));

  // Truncated header.
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(frame, cell::kCallAudioHeaderSize - 1U, callId,
                                              sizeof(callId), sequence, &payload,
                                              payloadLength));

  // Declared length larger than the real payload.
  memcpy(broken, frame, written);
  broken[15] = 0xFFU;
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(broken, written, callId, sizeof(callId), sequence,
                                              &payload, payloadLength));

  // Declared length smaller than the real payload.
  memcpy(broken, frame, written);
  broken[14] = 0x01U;
  broken[15] = 0x00U;
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(broken, written, callId, sizeof(callId), sequence,
                                              &payload, payloadLength));

  // Non alphanumeric call id.
  memcpy(broken, frame, written);
  broken[4] = ' ';
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(broken, written, callId, sizeof(callId), sequence,
                                              &payload, payloadLength));

  // Zero length payload.
  memcpy(broken, frame, written);
  broken[14] = 0x00U;
  broken[15] = 0x00U;
  TEST_ASSERT_FALSE(cell::parseCallAudioFrame(broken, written, callId, sizeof(callId), sequence,
                                              &payload, payloadLength));

  // A valid frame still parses after all those refusals.
  TEST_ASSERT_TRUE(cell::parseCallAudioFrame(frame, written, callId, sizeof(callId), sequence,
                                             &payload, payloadLength));
}

void testCallAudioFrameBuilderRejectsInvalidInput() {
  uint8_t pcm[64];
  memset(pcm, 0x22, sizeof(pcm));
  uint8_t frame[cell::kCallAudioHeaderSize + sizeof(pcm)] = {0};

  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(cell::buildCallAudioFrame(
                                   "bad", 1U, pcm, sizeof(pcm), frame, sizeof(frame))));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(cell::buildCallAudioFrame(
                                   kCallA, 1U, nullptr, sizeof(pcm), frame, sizeof(frame))));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(
                                   cell::buildCallAudioFrame(kCallA, 1U, pcm, 0U, frame,
                                                             sizeof(frame))));
  // Output buffer too small.
  TEST_ASSERT_EQUAL_UINT32(
      0U, static_cast<uint32_t>(cell::buildCallAudioFrame(
              kCallA, 1U, pcm, sizeof(pcm), frame,
              cell::kCallAudioHeaderSize + sizeof(pcm) - 1U)));
  // Payload above the explicit bound. The guard fires before any read, so the
  // short local buffer is never touched.
  TEST_ASSERT_EQUAL_UINT32(
      0U, static_cast<uint32_t>(cell::buildCallAudioFrame(
              kCallA, 1U, pcm, cell::kCallAudioMaxPayload + 1U, frame, sizeof(frame))));
}

// ---------------------------------------------------------------------------
// Structural guards: the call feature must never touch the RS485 transport
// ---------------------------------------------------------------------------
void testCallAudioMagicCannotBeMistakenForABusFrame() {
  // The bus protocol has a 1 byte version field and a 1 byte type field: a media
  // frame must be impossible to confuse with one of them.
  const uint8_t types[] = {
      cell::kTypePing,     cell::kTypePong,      cell::kTypeToken,
      cell::kTypeRouteState, cell::kTypeRouteRequest, cell::kTypeBusAck,
      cell::kTypeText,     cell::kTypeReceipt,   cell::kTypeVoiceBlock,
      cell::kTypeVoiceFeedback, cell::kTypeAppControl,
  };
  for (size_t i = 0; i < sizeof(types); ++i) {
    TEST_ASSERT_TRUE(cell::kCallAudioMagic != types[i]);
  }
  TEST_ASSERT_TRUE(cell::kCallAudioMagic != cell::kProtocolVersion);
  // The COBS delimiter (0x00) never appears inside a bus frame: a media frame
  // must not be confused with the framing delimiter either.
  TEST_ASSERT_TRUE(cell::kCallAudioMagic != 0x00U);
  TEST_ASSERT_TRUE(cell::kCallAudioVersion != 0x00U);
  // A media frame is bigger than any possible bus frame: it could not even be
  // encapsulated by accident.
  TEST_ASSERT_TRUE(cell::kCallAudioHeaderSize + cell::kCallAudioFrameBytes >
                   cell::kMaxRawFrameSize);
}

void testCallModuleDoesNotDependOnTheBusLibraries() {
  const std::string header = readSource("lib/cell_call/include/cell_call.h");
  const std::string source = readSource("lib/cell_call/src/cell_call.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "cell_call.h not found from the test working dir");
  TEST_ASSERT_TRUE_MESSAGE(!source.empty(), "cell_call.cpp not found from the test working dir");

  // The check targets real include directives: the header prose is allowed to
  // name the bus libraries to explain why it does not use them.
  const char* includes[] = {"#include \"cell_protocol.h\"", "#include <cell_protocol.h>",
                            "#include \"cell_core.h\"", "#include <cell_core.h>"};
  for (const char* directive : includes) {
    TEST_ASSERT_FALSE_MESSAGE(contains(header, directive), directive);
    TEST_ASSERT_FALSE_MESSAGE(contains(source, directive), directive);
  }

  // No bus concept may appear in the call module.
  const char* forbidden[] = {"FrameQueue", "BusArbiter", "AckTracker", "submitAppPayload",
                             "kTypeText",  "kTypeVoiceBlock", "cobsEncode", "crc16Ccitt"};
  for (const char* needle : forbidden) {
    TEST_ASSERT_FALSE_MESSAGE(contains(header, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(source, needle), needle);
  }
}

void testStationCallHandlersNeverSubmitToTheMesh() {
  const std::string server = readSource("src/common/app_server.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!server.empty(), "app_server.cpp not found from the test working dir");

  // The signalling handlers and the media relay are contiguous blocks in
  // app_server.cpp; neither may reach the mesh.
  const std::string signalling =
      between(server, "void AppServer::handleCallInvite()", "void AppServer::handleCallState()");
  TEST_ASSERT_TRUE_MESSAGE(!signalling.empty(), "call signalling handlers not found");
  TEST_ASSERT_FALSE(contains(signalling, "submitAppPayload"));

  const std::string media =
      between(server, "void AppServer::handleCallAudio(", "void AppServer::serviceCallTimeouts()");
  TEST_ASSERT_TRUE_MESSAGE(!media.empty(), "call media handler not found");
  TEST_ASSERT_FALSE(contains(media, "submitAppPayload"));
  TEST_ASSERT_FALSE(contains(media, "queue_."));
  TEST_ASSERT_FALSE(contains(media, "kTypeText"));
  TEST_ASSERT_FALSE(contains(media, "kTypeVoiceBlock"));

  // No new bus frame type was introduced for calls.
  TEST_ASSERT_FALSE(contains(server, "kTypeCall"));
  TEST_ASSERT_FALSE(contains(server, "kTypeAudio"));
}

void testMeshNodeAndProtocolWereNotTouchedByTheCallFeature() {
  const std::string nodeSource = readSource("src/common/mesh_node.cpp");
  const std::string nodeHeader = readSource("src/common/mesh_node.h");
  const std::string protocol = readSource("lib/cell_protocol/include/cell_protocol.h");
  TEST_ASSERT_TRUE_MESSAGE(!nodeSource.empty(), "mesh_node.cpp not found");
  TEST_ASSERT_TRUE_MESSAGE(!nodeHeader.empty(), "mesh_node.h not found");
  TEST_ASSERT_TRUE_MESSAGE(!protocol.empty(), "cell_protocol.h not found");

  const char* needles[] = {"CallTable", "kCallAudio", "handleCall", "call_audio", "cell_call.h"};
  for (const char* needle : needles) {
    TEST_ASSERT_FALSE_MESSAGE(contains(nodeSource, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(nodeHeader, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(protocol, needle), needle);
  }
}
