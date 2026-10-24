#include <unity.h>
#include <cstring>

#include "cell_core.h"

namespace {

cell::Frame frameWith(uint8_t type, uint16_t seq) {
  cell::Frame frame;
  frame.type = type;
  frame.srcNode = 1;
  frame.dstNode = 2;
  frame.seq = seq;
  frame.payloadLength = 0;
  return frame;
}

cell::BusArbiterConfig arbiterConfig(uint8_t nodeId) {
  cell::BusArbiterConfig config;
  config.nodeId = nodeId;
  config.holdMs = 100;
  config.tokenTimeoutMs = 400;
  config.claimSlotMs = 40;
  config.maxFramesPerTurn = 2;
  return config;
}

}  // namespace

void testQueueServesHighestPriorityFirst() {
  cell::FrameQueue queue;
  TEST_ASSERT_TRUE(queue.push(cell::FrameQueue::kPrioAudio, frameWith(cell::kTypeVoiceBlock, 1), 0));
  TEST_ASSERT_TRUE(queue.push(cell::FrameQueue::kPrioText, frameWith(cell::kTypeText, 2), 1));
  TEST_ASSERT_TRUE(queue.push(cell::FrameQueue::kPrioControl, frameWith(cell::kTypeToken, 3), 2));
  // Control traffic lives in its own ring, user frames in the application ring.
  TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(queue.size() + queue.controlSize()));

  cell::Frame out;
  TEST_ASSERT_TRUE(queue.pop(out, 3));
  TEST_ASSERT_EQUAL_UINT16(3, out.seq);
  TEST_ASSERT_TRUE(queue.pop(out, 3));
  TEST_ASSERT_EQUAL_UINT16(2, out.seq);
  TEST_ASSERT_TRUE(queue.pop(out, 3));
  TEST_ASSERT_EQUAL_UINT16(1, out.seq);
  TEST_ASSERT_TRUE(queue.empty());
  TEST_ASSERT_FALSE(queue.pop(out, 3));
}

void testQueueIsBoundedAndCountsDrops() {
  cell::FrameQueue queue;
  for (size_t i = 0; i < cell::kQueueCapacity; ++i) {
    TEST_ASSERT_TRUE(queue.push(cell::FrameQueue::kPrioText,
                                frameWith(cell::kTypeText, static_cast<uint16_t>(i)), 0));
  }
  TEST_ASSERT_FALSE(queue.push(cell::FrameQueue::kPrioText, frameWith(cell::kTypeText, 99), 0));
  TEST_ASSERT_EQUAL_UINT32(1U, queue.droppedCount());
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cell::kQueueCapacity),
                           static_cast<uint32_t>(queue.size()));
}

void testQueuePromotesStarvedAudio() {
  cell::FrameQueue queue;
  queue.push(cell::FrameQueue::kPrioAudio, frameWith(cell::kTypeVoiceBlock, 10), 0);
  queue.push(cell::FrameQueue::kPrioText, frameWith(cell::kTypeText, 20), 1900);

  queue.promoteStarved(2000, 1500);

  cell::Frame out;
  queue.pop(out, 2000);
  TEST_ASSERT_EQUAL_UINT16(10, out.seq);
  queue.pop(out, 2000);
  TEST_ASSERT_EQUAL_UINT16(20, out.seq);
}

void testArbiterNeedsTokenToTransmit() {
  cell::BusArbiter arbiter;
  arbiter.begin(arbiterConfig(2), 1000);
  TEST_ASSERT_FALSE(arbiter.hasToken());
  TEST_ASSERT_FALSE(arbiter.mayTransmit(1000));
  TEST_ASSERT_FALSE(arbiter.shouldHandOver(1000));

  arbiter.onTokenReceived(1000);
  TEST_ASSERT_TRUE(arbiter.hasToken());
  TEST_ASSERT_TRUE(arbiter.mayTransmit(1000));
  TEST_ASSERT_EQUAL_UINT32(1U, arbiter.turnCount());
}

void testArbiterHandsOverWhenBudgetExhausted() {
  cell::BusArbiter arbiter;
  arbiter.begin(arbiterConfig(1), 0);
  arbiter.setAliveMask(0x07);
  arbiter.onTokenReceived(0);

  arbiter.onFrameSent(10);
  arbiter.onFrameSent(20);
  TEST_ASSERT_FALSE(arbiter.mayTransmit(30));
  TEST_ASSERT_TRUE(arbiter.shouldHandOver(30));

  arbiter.onTokenSent(40);
  TEST_ASSERT_FALSE(arbiter.hasToken());

  // Holding too long also releases the token.
  arbiter.onTokenReceived(100);
  TEST_ASSERT_TRUE(arbiter.mayTransmit(100));
  TEST_ASSERT_FALSE(arbiter.mayTransmit(250));
  TEST_ASSERT_TRUE(arbiter.shouldHandOver(250));
}

void testArbiterClaimsTokenAfterSilence() {
  cell::BusArbiter arbiter;
  arbiter.begin(arbiterConfig(2), 1000);  // threshold = 400 + 2 * 40 = 480 ms
  arbiter.onBusActivity(1000);
  TEST_ASSERT_FALSE(arbiter.shouldClaim(1479));
  TEST_ASSERT_TRUE(arbiter.shouldClaim(1480));

  arbiter.claim(1480);
  TEST_ASSERT_TRUE(arbiter.hasToken());
  TEST_ASSERT_EQUAL_UINT32(1U, arbiter.tokenLostEvents());
}

void testArbiterSkipsDeadStations() {
  cell::BusArbiter arbiter;
  arbiter.begin(arbiterConfig(1), 0);
  arbiter.setAliveMask(0x03);  // stations 1 and 2 alive, station 3 silent
  TEST_ASSERT_EQUAL_UINT8(2, arbiter.nextNode(1));
  TEST_ASSERT_EQUAL_UINT8(1, arbiter.nextNode(3));
  TEST_ASSERT_EQUAL_UINT8(1, arbiter.nextAliveNode(2));

  arbiter.setAliveMask(0x05);  // station 2 is dead
  TEST_ASSERT_EQUAL_UINT8(3, arbiter.nextAliveNode(1));
  TEST_ASSERT_EQUAL_UINT8(1, arbiter.nextAliveNode(3));

  // A node alone keeps the token for itself instead of passing it nowhere.
  arbiter.setAliveMask(0x00);
  TEST_ASSERT_TRUE(arbiter.isAlive(1));
  TEST_ASSERT_EQUAL_UINT8(1, arbiter.nextAliveNode(1));
}

void testRoutingUpsertMovesUserBetweenStations() {
  cell::RoutingTable table;
  TEST_ASSERT_TRUE(table.upsert("u-anna", "Anna", 1, 1000));
  const cell::RouteEntry* entry = table.find("u-anna");
  TEST_ASSERT_NOT_NULL(entry);
  TEST_ASSERT_EQUAL_UINT8(1, entry->station);
  TEST_ASSERT_TRUE(entry->online);
  TEST_ASSERT_EQUAL_STRING("Anna", entry->displayName);

  // Same user seen on another station: the entry moves, it is never duplicated.
  TEST_ASSERT_TRUE(table.upsert("u-anna", "Anna", 3, 2000));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(table.entries()));
  TEST_ASSERT_EQUAL_UINT8(3, table.find("u-anna")->station);

  TEST_ASSERT_TRUE(table.remove("u-anna"));
  TEST_ASSERT_NULL(table.find("u-anna"));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.entries()));
}

void testRoutingExpiresSilentLocalUsers() {
  cell::RoutingTable table;
  table.upsert("u-local", "Local", 2, 1000);
  table.upsert("u-remote", "Remote", 1, 1000);

  table.expireLocal(2, 7000, 5000);
  TEST_ASSERT_FALSE(table.find("u-local")->online);
  TEST_ASSERT_TRUE(table.find("u-remote")->online);

  table.touch("u-local", 7100);
  TEST_ASSERT_TRUE(table.find("u-local")->online);
}

void testRoutingPresenceStateRoundtrip() {
  cell::RoutingTable source;
  source.upsert("u-anna", "Anna", 1, 1000);
  source.upsert("u-bruno", "Bruno", 1, 1000);
  source.upsert("u-elsewhere", "Elsewhere", 2, 1000);

  uint8_t buffer[cell::kMaxBusPayload] = {0};
  const size_t length = source.serializeStationState(1, buffer, sizeof(buffer));
  TEST_ASSERT_GREATER_THAN_UINT32(2U, static_cast<uint32_t>(length));
  TEST_ASSERT_EQUAL_UINT8(2, buffer[0]);

  cell::RoutingTable destination;
  TEST_ASSERT_TRUE(destination.applyStationState(1, buffer, length, 5000));
  TEST_ASSERT_EQUAL_UINT8(1, destination.find("u-anna")->station);
  TEST_ASSERT_TRUE(destination.find("u-anna")->online);
  TEST_ASSERT_TRUE(destination.find("u-bruno")->online);
  TEST_ASSERT_NULL(destination.find("u-elsewhere"));

  // A user who left the cell becomes offline on the next state publication.
  cell::RoutingTable updated;
  updated.upsert("u-anna", "Anna", 1, 1000);
  const size_t shorter = updated.serializeStationState(1, buffer, sizeof(buffer));
  TEST_ASSERT_TRUE(destination.applyStationState(1, buffer, shorter, 9000));
  TEST_ASSERT_TRUE(destination.find("u-anna")->online);
  TEST_ASSERT_FALSE(destination.find("u-bruno")->online);
}

void testRoutingTruncatedPresenceStateIsRejected() {
  cell::RoutingTable table;
  const uint8_t broken[] = {0x01, 0x10, 'u'};  // claims a 16 byte id, only 1 byte present
  TEST_ASSERT_FALSE(table.applyStationState(2, broken, sizeof(broken), 1000));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.entries()));
}

// Non-regression: a truncated announcement that is valid up to its last entry
// must not leave a half updated routing table (two phase validation).
void testRoutingPartialPresenceStateIsRejectedAtomically() {
  cell::RoutingTable table;
  table.upsert("u-anna", "Anna", 2, 1000);
  table.upsert("u-bruno", "Bruno", 2, 1000);

  // Claims two entries: the first one is complete, the second one is truncated.
  const uint8_t partial[] = {0x02, 0x06, 'u', '-', 'n', 'e', 'w', 0x03, 'N', 'e', 'w',
                             0x06, 'u', '-', 'b', 'r', 'u'};
  TEST_ASSERT_FALSE(table.applyStationState(2, partial, sizeof(partial), 2000));
  TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(table.entries()));
  TEST_ASSERT_NULL(table.find("u-new"));
  TEST_ASSERT_TRUE(table.find("u-anna")->online);
  TEST_ASSERT_TRUE(table.find("u-bruno")->online);
}

void testRoutingMarksWholeStationOffline() {
  cell::RoutingTable table;
  table.upsert("u-anna", "Anna", 3, 1000);
  table.upsert("u-bruno", "Bruno", 1, 1000);
  table.markStationOffline(3);
  TEST_ASSERT_FALSE(table.find("u-anna")->online);
  TEST_ASSERT_TRUE(table.find("u-bruno")->online);
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(table.countOnline(3)));
}

void testEnvelopeRoundtrip() {
  const uint8_t body[] = {'{', '"', 'k', '"', '}', 0x00, 'x'};
  uint8_t buffer[128] = {0};
  const size_t length = cell::buildAppEnvelope("u-anna", "u-bruno", body, sizeof(body),
                                               buffer, sizeof(buffer));
  TEST_ASSERT_GREATER_THAN_UINT32(0U, static_cast<uint32_t>(length));

  cell::AppEnvelope envelope;
  TEST_ASSERT_TRUE(cell::parseAppEnvelope(buffer, length, envelope));
  TEST_ASSERT_EQUAL_STRING("u-bruno", envelope.destination);
  TEST_ASSERT_EQUAL_STRING("u-anna", envelope.source);
  TEST_ASSERT_EQUAL_UINT32(sizeof(body), static_cast<uint32_t>(envelope.bodyLength));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(body, envelope.body, sizeof(body));
}

void testEnvelopeRejectsBadLengths() {
  cell::AppEnvelope envelope;
  const uint8_t truncated[] = {0x10, 'u'};
  TEST_ASSERT_FALSE(cell::parseAppEnvelope(truncated, sizeof(truncated), envelope));
  const uint8_t empty[] = {0x00, 0x00};
  TEST_ASSERT_FALSE(cell::parseAppEnvelope(empty, sizeof(empty), envelope));

  uint8_t tiny[4] = {0};
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(cell::buildAppEnvelope(
                                    "u-anna", "u-bruno", nullptr, 40, tiny, sizeof(tiny))));
}

namespace {

cell::Frame makeFragment(uint8_t index, uint8_t count, uint16_t messageSeq,
                         const uint8_t* data, size_t length) {
  cell::Frame frame;
  frame.type = cell::kTypeText;
  frame.srcNode = 1;
  frame.dstNode = 2;
  frame.flags = cell::kFlagFragmented;
  frame.payload[0] = index;
  frame.payload[1] = count;
  frame.payload[2] = static_cast<uint8_t>(messageSeq & 0xFFU);
  frame.payload[3] = static_cast<uint8_t>(messageSeq >> 8);
  memcpy(frame.payload + cell::kFragmentOverhead, data, length);
  frame.payloadLength = static_cast<uint16_t>(length + cell::kFragmentOverhead);
  return frame;
}

}  // namespace

void testFragmentAssemblyOutOfOrder() {
  uint8_t original[1000];
  for (size_t i = 0; i < sizeof(original); ++i) {
    original[i] = static_cast<uint8_t>((i * 13U) & 0xFFU);
  }

  const size_t chunk = cell::kFragmentBodySize;
  const uint8_t count = 3;
  cell::FragmentAssembler assembler;
  uint8_t assembled[cell::kMaxReassembledPayload] = {0};
  size_t assembledLength = 0;

  // Third fragment first, then the first one, then the second one.
  const uint8_t order[3] = {2, 0, 1};
  cell::FragmentAssembler::Status status = cell::FragmentAssembler::Status::kIncomplete;
  for (size_t i = 0; i < 3; ++i) {
    const uint8_t index = order[i];
    const size_t offset = index * chunk;
    const size_t remaining = sizeof(original) - offset;
    const size_t length = remaining < chunk ? remaining : chunk;
    const cell::Frame fragment = makeFragment(index, count, 900, original + offset, length);
    status = assembler.accept(fragment, 1000 + i, assembled, sizeof(assembled), assembledLength);
  }

  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kComplete),
                          static_cast<uint8_t>(status));
  TEST_ASSERT_EQUAL_UINT32(sizeof(original), static_cast<uint32_t>(assembledLength));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, assembled, sizeof(original));
}

void testFragmentDuplicateIsIgnored() {
  const uint8_t data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  const cell::Frame fragment = makeFragment(0, 2, 77, data, sizeof(data));

  cell::FragmentAssembler assembler;
  uint8_t out[64] = {0};
  size_t outLength = 0;

  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kIncomplete),
      static_cast<uint8_t>(assembler.accept(fragment, 10, out, sizeof(out), outLength)));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kIncomplete),
      static_cast<uint8_t>(assembler.accept(fragment, 11, out, sizeof(out), outLength)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kComplete),
                          static_cast<uint8_t>(assembler.accept(
                              makeFragment(1, 2, 77, data, sizeof(data)), 12, out, sizeof(out),
                              outLength)));
  TEST_ASSERT_EQUAL_UINT32(16U, static_cast<uint32_t>(outLength));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out, sizeof(data));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out + sizeof(data), sizeof(data));
}

void testFragmentRejectsInvalidIndex() {
  const uint8_t data[4] = {9, 9, 9, 9};
  cell::FragmentAssembler assembler;
  uint8_t out[64] = {0};
  size_t outLength = 0;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kError),
                          static_cast<uint8_t>(assembler.accept(
                              makeFragment(3, 3, 1, data, sizeof(data)), 0, out, sizeof(out),
                              outLength)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kError),
                          static_cast<uint8_t>(assembler.accept(
                              makeFragment(0, static_cast<uint8_t>(cell::kMaxFragments + 1U), 1,
                                           data, sizeof(data)),
                              0, out, sizeof(out), outLength)));
}

void testFragmentContextExpires() {
  const uint8_t data[4] = {1, 2, 3, 4};
  cell::FragmentAssembler assembler;
  uint8_t out[64] = {0};
  size_t outLength = 0;
  assembler.accept(makeFragment(0, 2, 5, data, sizeof(data)), 1000, out, sizeof(out), outLength);
  assembler.expire(9000, 5000);
  // The context is gone: the second fragment alone can never complete.
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kIncomplete),
      static_cast<uint8_t>(assembler.accept(makeFragment(1, 2, 5, data, sizeof(data)), 9100, out,
                                            sizeof(out), outLength)));
}
