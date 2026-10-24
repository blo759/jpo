// Tests of user identity and mobility between cells.
//
// The rule under test is the one the deployment depends on: a user is identified
// by its stable identifier, never by its name, its address or the cell it is
// attached to. Moving from one cell to another must therefore update its
// location and nothing else: no new user, no duplicate, no lost conversation.
//
// Every test uses the real RoutingTable of lib/cell_core, the same one the
// firmware runs, and the same presence announcement format that travels on the
// RS485 bus (serializeStationState / applyStationState).

#include <unity.h>

#include <cstring>

#include "cell_core.h"

namespace {

constexpr uint8_t kStationA = 1;
constexpr uint8_t kStationB = 2;
constexpr uint8_t kStationC = 3;
// Same value as MeshNode uses to expire a local user that stopped refreshing its
// presence (mesh_node.cpp). The user is then only set offline, never deleted.
constexpr uint32_t kLocalUserTimeoutMs = 90000U;

// Publishes the presence of `from` (as it is known on the bus) to `to`, exactly
// like MeshNode does when it broadcasts its route state.
void announce(const cell::RoutingTable& from,
              uint8_t fromStation,
              cell::RoutingTable& to,
              uint32_t nowMs) {
  uint8_t buffer[cell::kMaxBusPayload] = {0};
  const size_t length = from.serializeStationState(fromStation, buffer, sizeof(buffer));
  TEST_ASSERT_TRUE(length > 0U);
  TEST_ASSERT_TRUE(to.applyStationState(fromStation, buffer, length, nowMs));
}

size_t countUser(const cell::RoutingTable& table, const char* userId) {
  size_t found = 0;
  for (size_t i = 0; i < table.entries(); ++i) {
    if (strcmp(table.at(i).userId, userId) == 0) {
      ++found;
    }
  }
  return found;
}

}  // namespace

// A user that moves from cell A to cell B keeps its identifier and its name: only
// the station it is attached to changes.
void testMobilityKeepsTheIdentityAndTheNameWhenAUserChangesCell() {
  cell::RoutingTable cellA;
  cell::RoutingTable cellB;
  TEST_ASSERT_TRUE(cellA.upsert("user_123", "Abdou", kStationA, 1000U));

  // The user arrives on cell B. Its identifier and its announced name come from
  // the client, so cell B registers the SAME user.
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 2000U));

  // Cell A learns it from the announcement of cell B.
  announce(cellB, kStationB, cellA, 2100U);

  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellA.entries()));
  const cell::RouteEntry* moved = cellA.find("user_123");
  TEST_ASSERT_NOT_NULL(moved);
  TEST_ASSERT_EQUAL_STRING("user_123", moved->userId);
  TEST_ASSERT_EQUAL_STRING("Abdou", moved->displayName);
  TEST_ASSERT_EQUAL_UINT8(kStationB, moved->station);
  TEST_ASSERT_TRUE(moved->online);
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(countUser(cellA, "user_123")));
  // Cell A no longer counts it as one of its own users.
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(cellA.countOnline(kStationA)));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellA.countOnline(kStationB)));
}

// Receiving the same arrival event several times must not create several users.
void testMobilityIsIdempotentOnRepeatedAnnouncements() {
  cell::RoutingTable cellB;
  cell::RoutingTable cellC;
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 2000U));

  for (uint32_t i = 0; i < 5U; ++i) {
    announce(cellB, kStationB, cellC, 2100U + i);
  }
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellC.entries()));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(countUser(cellC, "user_123")));
  // Registering the same arrival again on the station itself is idempotent too.
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 3000U));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellB.entries()));
}

// A -> B -> C -> A: the identity survives every hop and the entry is always the
// same one, moved from station to station.
void testMobilitySurvivesARoundTripThroughEveryCell() {
  cell::RoutingTable cellA;
  cell::RoutingTable cellB;
  cell::RoutingTable cellC;
  TEST_ASSERT_TRUE(cellA.upsert("user_123", "Abdou", kStationA, 1000U));

  // A -> B
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 2000U));
  announce(cellB, kStationB, cellC, 2100U);
  announce(cellB, kStationB, cellA, 2100U);
  TEST_ASSERT_EQUAL_UINT8(kStationB, cellA.find("user_123")->station);
  TEST_ASSERT_EQUAL_UINT8(kStationB, cellC.find("user_123")->station);

  // B -> C
  TEST_ASSERT_TRUE(cellC.upsert("user_123", "Abdou", kStationC, 3000U));
  announce(cellC, kStationC, cellA, 3100U);
  announce(cellC, kStationC, cellB, 3100U);
  TEST_ASSERT_EQUAL_UINT8(kStationC, cellA.find("user_123")->station);
  TEST_ASSERT_EQUAL_UINT8(kStationC, cellB.find("user_123")->station);

  // C -> A, back to the beginning.
  TEST_ASSERT_TRUE(cellA.upsert("user_123", "Abdou", kStationA, 4000U));
  announce(cellA, kStationA, cellB, 4100U);
  announce(cellA, kStationA, cellC, 4100U);

  cell::RoutingTable* tables[] = {&cellA, &cellB, &cellC};
  for (cell::RoutingTable* table : tables) {
    TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(table->entries()));
    const cell::RouteEntry* entry = table->find("user_123");
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_STRING("Abdou", entry->displayName);
    TEST_ASSERT_EQUAL_UINT8(kStationA, entry->station);
    TEST_ASSERT_TRUE(entry->online);
  }
}

// Two different people may legitimately share a display name: they must stay two
// distinct users, and their messages must not be mixed up.
void testMobilityTwoUsersWithTheSameNameStayDistinct() {
  cell::RoutingTable cellA;
  cell::RoutingTable cellB;
  TEST_ASSERT_TRUE(cellA.upsert("user_123", "Abdou", kStationA, 1000U));
  TEST_ASSERT_TRUE(cellB.upsert("user_456", "Abdou", kStationB, 1000U));

  announce(cellB, kStationB, cellA, 1100U);

  TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(cellA.entries()));
  TEST_ASSERT_EQUAL_UINT8(kStationA, cellA.find("user_123")->station);
  TEST_ASSERT_EQUAL_UINT8(kStationB, cellA.find("user_456")->station);
  TEST_ASSERT_EQUAL_STRING("Abdou", cellA.find("user_123")->displayName);
  TEST_ASSERT_EQUAL_STRING("Abdou", cellA.find("user_456")->displayName);
}

// A user that leaves a cell is set offline there, it is never deleted: a
// temporary disconnection must not destroy an identity, and the entry keeps the
// name and the last known station.
void testMobilityALeavingUserGoesOfflineButIsNeverDeleted() {
  cell::RoutingTable cellA;
  TEST_ASSERT_TRUE(cellA.upsert("user_123", "Abdou", kStationA, 1000U));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellA.countOnline(kStationA)));

  // The phone stops refreshing its presence.
  cellA.expireLocal(kStationA, 1000U + kLocalUserTimeoutMs + 1U, kLocalUserTimeoutMs);

  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellA.entries()));
  TEST_ASSERT_EQUAL_STRING("Abdou", cellA.find("user_123")->displayName);
  TEST_ASSERT_EQUAL_UINT8(kStationA, cellA.find("user_123")->station);
  TEST_ASSERT_FALSE(cellA.find("user_123")->online);
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(cellA.countOnline(kStationA)));

  // Coming back to the same cell restores the very same entry.
  TEST_ASSERT_TRUE(cellA.touch("user_123", 2000U + kLocalUserTimeoutMs));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellA.entries()));
  TEST_ASSERT_TRUE(cellA.find("user_123")->online);
  TEST_ASSERT_EQUAL_STRING("Abdou", cellA.find("user_123")->displayName);
}

// When a station becomes unreachable, its users are marked offline but their
// entries are kept: the conversations and the routing of a returning user do not
// depend on a station being up.
void testMobilityAnUnreachableStationDoesNotEraseItsUsers() {
  cell::RoutingTable cellC;
  TEST_ASSERT_TRUE(cellC.upsert("user_123", "Abdou", kStationB, 1000U));
  cellC.markStationOffline(kStationB);

  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellC.entries()));
  TEST_ASSERT_EQUAL_STRING("Abdou", cellC.find("user_123")->displayName);
  TEST_ASSERT_EQUAL_UINT8(kStationB, cellC.find("user_123")->station);
  TEST_ASSERT_FALSE(cellC.find("user_123")->online);

  // The station returns and announces itself again: same entry, online again.
  cell::RoutingTable cellB;
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 5000U));
  announce(cellB, kStationB, cellC, 5100U);
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellC.entries()));
  TEST_ASSERT_TRUE(cellC.find("user_123")->online);
  TEST_ASSERT_EQUAL_STRING("Abdou", cellC.find("user_123")->displayName);
}

// A presence announcement received out of order must not leave the network with a
// wrong view for ever: the hosting cell republishes its presence as soon as it
// registers or loses a user, and the last announcement wins. This test documents
// the exact convergence property of the current design.
void testMobilityConvergesAfterAStaleAnnouncement() {
  cell::RoutingTable cellA;
  cell::RoutingTable cellB;
  cell::RoutingTable cellC;
  TEST_ASSERT_TRUE(cellA.upsert("user_123", "Abdou", kStationA, 1000U));

  // Cell A announces its presence; the frame is still in flight when the user
  // moves to B.
  uint8_t staleBuffer[cell::kMaxBusPayload] = {0};
  const size_t staleLength =
      cellA.serializeStationState(kStationA, staleBuffer, sizeof(staleBuffer));
  TEST_ASSERT_TRUE(staleLength > 0U);

  // B takes the user over and publishes immediately.
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 2000U));
  announce(cellB, kStationB, cellC, 2100U);
  TEST_ASSERT_EQUAL_UINT8(kStationB, cellC.find("user_123")->station);

  // The stale frame of A arrives late and moves the entry back: this is the known
  // transient of a last-announcement-wins table.
  TEST_ASSERT_TRUE(cellC.applyStationState(kStationA, staleBuffer, staleLength, 2200U));
  TEST_ASSERT_EQUAL_UINT8(kStationA, cellC.find("user_123")->station);

  // The next announcement of the hosting cell corrects it, and there is still
  // exactly one entry for that user.
  announce(cellB, kStationB, cellC, 2300U);
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellC.entries()));
  TEST_ASSERT_EQUAL_UINT8(kStationB, cellC.find("user_123")->station);
  TEST_ASSERT_EQUAL_STRING("Abdou", cellC.find("user_123")->displayName);
}

// The station of the user is the only thing that may change: the identifier is
// what the Android application keeps in its preferences, so it must survive a
// move, a reconnection and a rename.
void testMobilityTheIdentifierIsTheIdentityAndNeverTheName() {
  cell::RoutingTable cellB;
  // The name is only a label: the identifier is the key.
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou", kStationB, 1000U));
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "Abdou Diallo", kStationB, 2000U));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellB.entries()));
  TEST_ASSERT_EQUAL_STRING("Abdou Diallo", cellB.find("user_123")->displayName);

  // A blank name never creates a second entry and never erases the identifier:
  // the table falls back to the identifier itself.
  TEST_ASSERT_TRUE(cellB.upsert("user_123", "", kStationB, 3000U));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellB.entries()));
  TEST_ASSERT_EQUAL_STRING("user_123", cellB.find("user_123")->displayName);
  TEST_ASSERT_NOT_NULL(cellB.find("user_123"));

  // An invalid identifier is refused instead of being turned into a user.
  TEST_ASSERT_FALSE(cellB.upsert("", "Abdou", kStationB, 4000U));
  TEST_ASSERT_FALSE(cellB.upsert(nullptr, "Abdou", kStationB, 4000U));
  TEST_ASSERT_FALSE(cellB.upsert("user_999", "Abdou", 0U, 4000U));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(cellB.entries()));
}
