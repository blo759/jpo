// Reliability tests: acknowledgement tracking, protected queueing, token idle
// rounds, long fragment reassembly and a deterministic two sender simulation.
//
// Everything here is platform neutral: the same code runs with `pio test -e
// native` and on the ESP32 target.

#include <unity.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "cell_core.h"
#include "cell_protocol.h"

namespace {

cell::Frame makeDataFrame(uint8_t src, uint8_t dst, uint16_t seq, uint8_t flags,
                          uint8_t marker = 0) {
  cell::Frame frame;
  frame.type = cell::kTypeText;
  frame.srcNode = src;
  frame.dstNode = dst;
  frame.seq = seq;
  frame.flags = flags;
  frame.payload[0] = marker;
  frame.payloadLength = 1;
  return frame;
}

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

// ---------------------------------------------------------------------------
// AckTracker
// ---------------------------------------------------------------------------
void testAckTrackerClearsOnlyTheAcknowledgedFragment() {
  cell::AckTracker tracker;
  const cell::Frame first = makeDataFrame(1, 2, 100, cell::kFlagAckRequired);
  cell::Frame second = makeDataFrame(1, 2, 101, cell::kFlagAckRequired);
  cell::Frame third = makeDataFrame(1, 2, 102, cell::kFlagAckRequired);

  TEST_ASSERT_TRUE(tracker.track(first, 100, 0, 400));
  TEST_ASSERT_TRUE(tracker.track(second, 100, 0, 400));
  TEST_ASSERT_TRUE(tracker.track(third, 100, 0, 400));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(tracker.pendingMessages()));
  TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(tracker.pendingFragments()));

  // An acknowledgement for another node must not clear anything.
  TEST_ASSERT_FALSE(tracker.onAck(3, 101));
  TEST_ASSERT_FALSE(tracker.onAck(2, 999));

  TEST_ASSERT_TRUE(tracker.onAck(2, 101));
  TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(tracker.pendingFragments()));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(tracker.pendingMessages()));

  // A duplicated acknowledgement is idempotent.
  TEST_ASSERT_FALSE(tracker.onAck(2, 101));
  TEST_ASSERT_EQUAL_UINT32(2U, static_cast<uint32_t>(tracker.pendingFragments()));

  TEST_ASSERT_TRUE(tracker.onAck(2, 100));
  TEST_ASSERT_TRUE(tracker.onAck(2, 102));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(tracker.pendingMessages()));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(tracker.pendingFragments()));
}

void testAckTrackerRetransmitsOnlyTheMissingFragment() {
  cell::AckTracker tracker;
  cell::Frame first = makeDataFrame(1, 3, 10, cell::kFlagAckRequired, 0x11);
  cell::Frame second = makeDataFrame(1, 3, 11, cell::kFlagAckRequired, 0x22);
  cell::Frame third = makeDataFrame(1, 3, 12, cell::kFlagAckRequired, 0x33);
  tracker.track(first, 10, 0, 100);
  tracker.track(second, 10, 0, 100);
  tracker.track(third, 10, 0, 100);

  TEST_ASSERT_TRUE(tracker.onAck(3, 10));
  TEST_ASSERT_TRUE(tracker.onAck(3, 12));

  cell::Frame retry;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::AckTracker::Action::kRetransmit),
                          static_cast<uint8_t>(tracker.poll(200, false, retry)));
  TEST_ASSERT_EQUAL_UINT16(11, retry.seq);
  TEST_ASSERT_EQUAL_UINT8(0x22, retry.payload[0]);
  TEST_ASSERT_TRUE((retry.flags & cell::kFlagRetransmit) != 0U);
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.retransmissions());
}

void testAckTrackerPostponesWhileThePeerIsAlive() {
  cell::AckTracker tracker;
  const cell::Frame frame = makeDataFrame(1, 2, 7, cell::kFlagAckRequired);
  tracker.track(frame, 7, 0, 100);

  cell::Frame retry;
  // Timeout reached but the peer emitted a frame after our send: it is alive and
  // working on our frame, so no duplicate is produced.
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::AckTracker::Action::kIdle),
                          static_cast<uint8_t>(tracker.poll(500, 450, retry)));
  TEST_ASSERT_EQUAL_UINT32(0U, tracker.retransmissions());
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(tracker.pendingMessages()));

  // Real silence after the postponement: the frame is retransmitted once.
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::AckTracker::Action::kRetransmit),
                          static_cast<uint8_t>(tracker.poll(700, 0, retry)));
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.retransmissions());
}

void testAckTrackerFailsOnlyAfterAllAttempts() {
  cell::AckTracker tracker;
  const cell::Frame frame = makeDataFrame(1, 2, 5, cell::kFlagAckRequired);
  tracker.track(frame, 5, 0, 100);

  cell::Frame retry;
  uint32_t now = 100;
  uint8_t retransmissions = 0;
  for (uint8_t attempt = 1; attempt < cell::kMaxAckAttempts; ++attempt) {
    now += 5000;  // far beyond the backoff
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::AckTracker::Action::kRetransmit),
                            static_cast<uint8_t>(tracker.poll(now, 0, retry)));
    ++retransmissions;
  }
  now += 10000;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::AckTracker::Action::kFailed),
                          static_cast<uint8_t>(tracker.poll(now, 0, retry)));
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.failures());
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(tracker.pendingMessages()));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(retransmissions), tracker.retransmissions());
}

void testAckTrackerRejectsWhenFullAndCountsIt() {
  cell::AckTracker tracker;
  for (size_t i = 0; i < cell::kMaxAckMessages; ++i) {
    const cell::Frame frame = makeDataFrame(1, static_cast<uint8_t>(2U + i),
                                            static_cast<uint16_t>(i), cell::kFlagAckRequired);
    TEST_ASSERT_TRUE(tracker.track(frame, static_cast<uint16_t>(i), 0, 400));
  }
  const cell::Frame extra = makeDataFrame(1, 9, 999, cell::kFlagAckRequired);
  TEST_ASSERT_FALSE(tracker.track(extra, 999, 0, 400));
  TEST_ASSERT_EQUAL_UINT32(1U, tracker.rejected());
}

// ---------------------------------------------------------------------------
// FrameQueue: control traffic has its own capacity
// ---------------------------------------------------------------------------
void testControlQueueNeverTakesThePlaceOfUserMessages() {
  cell::FrameQueue queue;
  for (size_t i = 0; i < cell::kQueueCapacity; ++i) {
    TEST_ASSERT_TRUE(queue.push(cell::FrameQueue::kPrioAudio,
                                makeDataFrame(1, 2, static_cast<uint16_t>(i), 0), 0));
  }
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.freeSlots()));

  // The application ring is full, but a bus acknowledgement still has room: it
  // uses the control ring and no user message is sacrificed.
  TEST_ASSERT_TRUE(queue.pushProtected(cell::FrameQueue::kPrioReceipt,
                                       makeDataFrame(1, 2, 500, 0), 0));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cell::kQueueCapacity),
                           static_cast<uint32_t>(queue.pendingOf(cell::FrameQueue::kPrioAudio)));
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(queue.controlSize()));
  TEST_ASSERT_EQUAL_UINT32(0U, queue.controlDroppedCount());

  cell::Frame out;
  TEST_ASSERT_TRUE(queue.pop(out, 0));
  TEST_ASSERT_EQUAL_UINT16(500, out.seq);  // the receipt is served first
  TEST_ASSERT_TRUE(queue.pop(out, 0));
  TEST_ASSERT_EQUAL_UINT16(0, out.seq);  // then the oldest audio frame
}

void testControlQueueIsBoundedAndReported() {
  cell::FrameQueue queue;
  for (size_t i = 0; i < cell::kControlQueueCapacity; ++i) {
    TEST_ASSERT_TRUE(queue.pushProtected(cell::FrameQueue::kPrioReceipt,
                                        makeDataFrame(1, 2, static_cast<uint16_t>(i), 0), 0));
  }
  TEST_ASSERT_FALSE(queue.pushProtected(cell::FrameQueue::kPrioReceipt,
                                        makeDataFrame(1, 2, 42, 0), 0));
  TEST_ASSERT_EQUAL_UINT32(1U, queue.controlDroppedCount());
  TEST_ASSERT_EQUAL_UINT32(1U, queue.droppedCount());
  // The application ring was never involved.
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(queue.size()));
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(cell::kQueueCapacity),
                           static_cast<uint32_t>(queue.freeSlots()));
}

// ---------------------------------------------------------------------------
// BusArbiter idle rounds and fairness
// ---------------------------------------------------------------------------
void testArbiterCountsIdleRounds() {
  cell::BusArbiter arbiter;
  cell::BusArbiterConfig config;
  config.nodeId = 2;
  arbiter.begin(config, 0);
  arbiter.setAliveMask(0x07);

  arbiter.takeToken(0, 0);
  TEST_ASSERT_EQUAL_UINT32(0U, arbiter.idlePasses());
  arbiter.noteIdlePass();
  arbiter.noteIdlePass();
  TEST_ASSERT_EQUAL_UINT32(2U, arbiter.idlePasses());
  TEST_ASSERT_EQUAL_UINT32(3U, arbiter.aliveCount());

  arbiter.giveTokenAway(10);
  TEST_ASSERT_EQUAL_UINT32(1U, arbiter.idleRounds());
  TEST_ASSERT_EQUAL_UINT32(0U, arbiter.idlePasses());
}

void testArbiterRotatesOverAliveNodesAndExposesBudget() {
  cell::BusArbiter arbiter;
  cell::BusArbiterConfig config;
  config.nodeId = 1;
  config.maxFramesPerTurn = 4;
  arbiter.begin(config, 0);
  arbiter.setAliveMask(0x07);

  TEST_ASSERT_EQUAL_UINT8(4, arbiter.maxFramesPerTurn());
  TEST_ASSERT_EQUAL_UINT8(2, arbiter.nextAliveNode(1));
  TEST_ASSERT_EQUAL_UINT8(3, arbiter.nextAliveNode(2));
  TEST_ASSERT_EQUAL_UINT8(1, arbiter.nextAliveNode(3));

  // A dead station is skipped without breaking the rotation.
  arbiter.setAliveMask(0x05);  // nodes 1 and 3 only
  TEST_ASSERT_EQUAL_UINT8(3, arbiter.nextAliveNode(1));
  TEST_ASSERT_EQUAL_UINT8(1, arbiter.nextAliveNode(3));
  TEST_ASSERT_EQUAL_UINT8(2, arbiter.aliveCount());
}

// ---------------------------------------------------------------------------
// Long messages: twelve fragments, concurrent messages
// ---------------------------------------------------------------------------
void testFragmentAssemblyTwelveFragmentsOutOfOrder() {
  uint8_t original[cell::kMaxReassembledPayload];
  for (size_t i = 0; i < sizeof(original); ++i) {
    original[i] = static_cast<uint8_t>((i * 31U + 7U) & 0xFFU);
  }
  const uint8_t count = cell::kMaxFragments;
  const size_t chunk = cell::kFragmentBodySize;

  cell::FragmentAssembler assembler;
  uint8_t assembled[cell::kMaxReassembledPayload] = {0};
  size_t assembledLength = 0;

  // Deterministic but messy order: even indexes first, then odd ones reversed.
  uint8_t order[cell::kMaxFragments] = {0};
  size_t cursor = 0;
  for (uint8_t index = 0; index < count; index = static_cast<uint8_t>(index + 2U)) {
    order[cursor++] = index;
  }
  for (int index = static_cast<int>(count) - 1; index >= 0; index -= 2) {
    order[cursor++] = static_cast<uint8_t>(index);
  }
  TEST_ASSERT_EQUAL_UINT32(count, static_cast<uint32_t>(cursor));

  cell::FragmentAssembler::Status status = cell::FragmentAssembler::Status::kIncomplete;
  for (uint8_t step = 0; step < count; ++step) {
    const uint8_t index = order[step];
    const size_t offset = static_cast<size_t>(index) * chunk;
    const size_t remaining = sizeof(original) - offset;
    const size_t length = remaining < chunk ? remaining : chunk;
    status = assembler.accept(makeFragment(index, count, 4242, original + offset, length),
                              1000 + step, assembled, sizeof(assembled), assembledLength);
  }
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kComplete),
                          static_cast<uint8_t>(status));
  TEST_ASSERT_EQUAL_UINT32(sizeof(original), static_cast<uint32_t>(assembledLength));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, assembled, sizeof(original));
}

void testFragmentAssemblyKeepsTwoConcurrentMessagesApart() {
  const uint8_t first[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  const uint8_t second[16] = {21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36};

  cell::FragmentAssembler assembler;
  uint8_t out[128] = {0};
  size_t outLength = 0;

  // Interleaved arrival of two different messages from the same station.
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kIncomplete),
      static_cast<uint8_t>(assembler.accept(makeFragment(0, 2, 900, first, 8), 0, out,
                                            sizeof(out), outLength)));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kIncomplete),
      static_cast<uint8_t>(assembler.accept(makeFragment(0, 2, 901, second, 8), 1, out,
                                            sizeof(out), outLength)));
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kComplete),
      static_cast<uint8_t>(assembler.accept(makeFragment(1, 2, 900, first + 8, 8), 2, out,
                                            sizeof(out), outLength)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first, out, 16);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kComplete),
      static_cast<uint8_t>(assembler.accept(makeFragment(1, 2, 901, second + 8, 8), 3, out,
                                            sizeof(out), outLength)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(second, out, 16);
}

// Non-regression of the delivery path: the reassembled message is exposed as a
// zero copy view whose length can exceed one bus frame (up to 5712 bytes). The
// previous implementation copied it into Frame::payload (480 bytes), which
// overflowed the stack of the loop task for any message larger than one frame.
void testFragmentAssemblyExposesTheFullPayloadWithoutCopy() {
  const size_t chunk = cell::kFragmentBodySize;
  const uint8_t count = cell::kMaxFragments;
  uint8_t original[cell::kMaxReassembledPayload];
  for (size_t i = 0; i < sizeof(original); ++i) {
    original[i] = static_cast<uint8_t>((i * 17U + 5U) & 0xFFU);
  }
  TEST_ASSERT_TRUE(sizeof(original) > cell::kMaxBusPayload);

  cell::FragmentAssembler assembler;
  uint8_t out[cell::kMaxReassembledPayload] = {0};
  size_t outLength = 0;
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(assembler.lastLength()));
  TEST_ASSERT_NULL(assembler.lastPayload());

  cell::FragmentAssembler::Status status = cell::FragmentAssembler::Status::kIncomplete;
  for (uint8_t index = 0; index < count; ++index) {
    const size_t offset = static_cast<size_t>(index) * chunk;
    const size_t remaining = sizeof(original) - offset;
    const size_t length = remaining < chunk ? remaining : chunk;
    status = assembler.accept(makeFragment(index, count, 77, original + offset, length),
                              1000 + index, out, sizeof(out), outLength);
  }
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kComplete),
                          static_cast<uint8_t>(status));
  TEST_ASSERT_EQUAL_UINT32(sizeof(original), static_cast<uint32_t>(outLength));

  // The view holds exactly the reassembled message, without any copy step.
  TEST_ASSERT_EQUAL_UINT32(sizeof(original), static_cast<uint32_t>(assembler.lastLength()));
  TEST_ASSERT_NOT_NULL(assembler.lastPayload());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(original, assembler.lastPayload(), sizeof(original));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(assembler.lastPayload(), out, sizeof(original));
  TEST_ASSERT_EQUAL_UINT32(1U, assembler.completedCount());
}

// A fragment that does not complete the message (or is rejected) must never
// publish a stale view of the previous message.
void testFragmentAssemblyClearsTheViewOnIncompleteAndRejectedInput() {
  const uint8_t data[4] = {1, 2, 3, 4};
  cell::FragmentAssembler assembler;
  uint8_t out[cell::kMaxReassembledPayload] = {0};
  size_t outLength = 0;

  assembler.accept(makeFragment(0, 2, 5, data, sizeof(data)), 0, out, sizeof(out), outLength);
  assembler.accept(makeFragment(1, 2, 5, data, sizeof(data)), 1, out, sizeof(out), outLength);
  TEST_ASSERT_EQUAL_UINT32(8U, static_cast<uint32_t>(assembler.lastLength()));

  // Incomplete fragment: the view of the previous message is cleared.
  assembler.accept(makeFragment(0, 2, 6, data, sizeof(data)), 2, out, sizeof(out), outLength);
  TEST_ASSERT_NULL(assembler.lastPayload());
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(assembler.lastLength()));

  // Rejected fragment: same contract.
  assembler.accept(makeFragment(0, 2, 6, data, sizeof(data)), 3, out, sizeof(out), outLength);
  const cell::Frame invalid = makeFragment(9, 2, 6, data, sizeof(data));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FragmentAssembler::Status::kError),
                          static_cast<uint8_t>(assembler.accept(invalid, 4, out, sizeof(out),
                                                                outLength)));
  TEST_ASSERT_NULL(assembler.lastPayload());
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(assembler.lastLength()));
}

void testFragmentAssemblyRejectsOutputBufferTooSmall() {
  const uint8_t data[16] = {0};
  cell::FragmentAssembler assembler;
  uint8_t out[8] = {0};
  size_t outLength = 0;
  assembler.accept(makeFragment(0, 2, 1, data, 16), 0, out, sizeof(out), outLength);
  TEST_ASSERT_EQUAL_UINT8(
      static_cast<uint8_t>(cell::FragmentAssembler::Status::kError),
      static_cast<uint8_t>(assembler.accept(makeFragment(1, 2, 1, data, 16), 1, out,
                                            sizeof(out), outLength)));
}

// ---------------------------------------------------------------------------
// Deterministic two sender simulation
// ---------------------------------------------------------------------------
// Models the real scheduling policy (token ring, token attached to the last data
// frame of a turn, idle passes instead of dropping the token, per fragment
// acknowledgements, selective retransmission) over a shared wire where one frame
// takes kSimTxMs. Nothing here touches hardware: it is a deterministic replay of
// the policy that runs in `mesh_node.cpp`.
namespace {

constexpr uint32_t kSimTxMs = 4;          // wire time of one frame
constexpr uint32_t kSimAckTimeout = 400;  // same value as the firmware
bool gSimTrace = false;                   // set to true to dump the event trace

/**
 * Optional trace of the simulated bus, disabled by default. It prints one line
 * per transmission, reception, retransmission and token event, which is what
 * makes a scheduling problem readable without hardware.
 */
void simTrace(const char* format, ...) {
  if (!gSimTrace) {
    return;
  }
  char buffer[192];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  printf("%s\n", buffer);
}

struct SimNode {
  uint8_t id = 1;
  uint8_t peer = 2;
  bool muted = false;  // simulates a station that is switched off / rebooting
  cell::FrameQueue queue;
  cell::BusArbiter arbiter;
  cell::AckTracker acks;
  bool transmitting = false;
  uint32_t txUntilMs = 0;
  uint32_t lastRxMs = 0;  // time of the last frame received from the bus
  cell::Frame txFrame;
  uint8_t delivered = 0;
  uint8_t lastDeliveredMarker = 0;
  uint32_t lastDeliveredSeq = 0;
  uint32_t framesSent = 0;
};

struct Sim {
  SimNode nodes[2];
  uint32_t now = 0;
  uint32_t busLastActivity = 0;

  size_t indexOf(uint8_t nodeId) const { return nodes[0].id == nodeId ? 0U : 1U; }

  void deliver(const SimNode& from, const cell::Frame& frame) {
    SimNode& target = nodes[indexOf(frame.dstNode)];
    if (target.muted) {
      return;  // the station is off: the frame is lost on the wire
    }
    target.lastRxMs = now;
    simTrace("%5u  node%u <- %s seq=%u from=%u", now, target.id,
             cell::messageTypeName(frame.type), static_cast<unsigned>(frame.seq), frame.srcNode);
    if (frame.type == cell::kTypeBusAck) {
      const uint16_t acked = static_cast<uint16_t>(frame.payload[0]) |
                             (static_cast<uint16_t>(frame.payload[1]) << 8);
      target.acks.onAck(frame.srcNode, acked);
    } else {
      target.arbiter.onBusActivity(now);  // the receiver only sees bus activity
      if ((frame.flags & cell::kFlagAckRequired) != 0U) {
        cell::Frame ack;
        ack.type = cell::kTypeBusAck;
        ack.srcNode = frame.dstNode;
        ack.dstNode = frame.srcNode;
        ack.payload[0] = static_cast<uint8_t>(frame.seq & 0xFFU);
        ack.payload[1] = static_cast<uint8_t>(frame.seq >> 8);
        ack.payloadLength = 2;
        target.queue.pushProtected(cell::FrameQueue::kPrioReceipt, ack, now);
      }
      if (frame.type == cell::kTypeText) {
        ++target.delivered;
        target.lastDeliveredMarker = frame.payload[0];
        target.lastDeliveredSeq = frame.seq;
      }
    }
    // The token may be attached to any frame, including a bus acknowledgement.
    if ((frame.flags & cell::kFlagTokenPass) != 0U) {
      target.arbiter.takeToken(now, 0);
      simTrace("%5u  node%u <- TOKEN (attached)", now, target.id);
    }
    (void)from;
  }

  void transmit(SimNode& node, const cell::Frame& frame) {
    node.txFrame = frame;
    node.transmitting = true;
    node.txUntilMs = now + kSimTxMs;
    ++node.framesSent;
    simTrace("%5u  node%u -> %s seq=%u dst=%u flags=0x%02X", now, node.id,
             cell::messageTypeName(frame.type), static_cast<unsigned>(frame.seq), frame.dstNode,
             frame.flags);
  }
};

// One 1 ms tick of the whole bus: completion, retransmission, transmission,
// token pass and token claim, in the same order as MeshNode::loop().
void simStep(Sim& sim) {
  for (size_t i = 0; i < 2; ++i) {
    SimNode& node = sim.nodes[i];
    if (node.muted) {
      continue;  // a switched off station neither transmits nor claims
    }
    if (node.transmitting && sim.now >= node.txUntilMs) {
      node.transmitting = false;
      sim.busLastActivity = sim.now;
      const cell::Frame finished = node.txFrame;
      if (finished.type == cell::kTypeToken) {
        sim.nodes[sim.indexOf(finished.dstNode)].arbiter.takeToken(sim.now, finished.payload[1]);
        node.arbiter.giveTokenAway(sim.now);
      } else {
        // The sender really put a frame on the wire: its turn budget is consumed.
        node.arbiter.noteDataFrame(sim.now);
        if (finished.dstNode == node.peer) {
          sim.deliver(node, finished);
          if ((finished.flags & cell::kFlagTokenPass) != 0U) {
            node.arbiter.giveTokenAway(sim.now);
          }
        }
      }
    }

    if (!node.transmitting) {
      cell::Frame retry;
      if (node.acks.poll(sim.now, node.lastRxMs, retry) == cell::AckTracker::Action::kRetransmit) {
        node.queue.pushProtected(cell::FrameQueue::kPrioControl, retry, sim.now);
      }
    }

    if (!node.transmitting && node.arbiter.hasToken()) {
      if (!node.queue.empty()) {
        if (node.arbiter.mayTransmit(sim.now)) {
          cell::Frame frame;
          if (node.queue.pop(frame, sim.now)) {
            const bool unicast = frame.dstNode != cell::kNodeBroadcast;
            const bool lastOfTurn =
                node.queue.empty() ||
                (node.arbiter.framesThisTurn() + 1U >= node.arbiter.maxFramesPerTurn());
            if (lastOfTurn && unicast) {
              frame.flags = static_cast<uint8_t>(frame.flags | cell::kFlagTokenPass);
            }
            if ((frame.flags & cell::kFlagAckRequired) != 0U &&
                (frame.flags & cell::kFlagRetransmit) == 0U) {
              node.acks.track(frame, frame.seq, sim.now, kSimAckTimeout);
            }
            sim.transmit(node, frame);
          }
        } else {
          // Turn budget exhausted while work remains: give the next station a
          // slot (this is what keeps both directions flowing under load).
          const uint8_t next = node.arbiter.nextAliveNode(node.id);
          if (next == node.id) {
            node.arbiter.takeToken(sim.now, 0);  // alone on the bus
          } else {
            SimNode& target = sim.nodes[sim.indexOf(next)];
            if (!target.transmitting) {
              cell::Frame token;
              token.type = cell::kTypeToken;
              token.srcNode = node.id;
              token.dstNode = next;
              token.payload[0] = next;
              token.payload[1] = 0;
              token.payloadLength = 2;
              sim.transmit(node, token);
              node.arbiter.giveTokenAway(sim.now);
            }
          }
        }
      } else {
        // Nothing to send: pass the token to the peer and only release it once
        // every alive station had an empty turn.
        SimNode& target = sim.nodes[sim.indexOf(node.peer)];
        if (!target.transmitting) {
          if (static_cast<uint8_t>(node.arbiter.idlePasses() + 1U) >= node.arbiter.aliveCount()) {
            node.arbiter.giveTokenAway(sim.now);
          } else {
            node.arbiter.noteIdlePass();
            cell::Frame token;
            token.type = cell::kTypeToken;
            token.srcNode = node.id;
            token.dstNode = node.peer;
            token.payload[0] = node.peer;
            token.payload[1] = node.arbiter.idlePasses();
            token.payloadLength = 2;
            sim.transmit(node, token);
          }
        }
      }
    }

    if (!node.transmitting && !node.arbiter.hasToken() && !node.queue.empty() &&
        node.arbiter.shouldClaim(sim.now)) {
      node.arbiter.claim(sim.now);
    }
  }
  ++sim.now;
}

void simSetup(Sim& sim, uint8_t idA, uint8_t idB) {
  sim.nodes[0].id = idA;
  sim.nodes[0].peer = idB;
  sim.nodes[1].id = idB;
  sim.nodes[1].peer = idA;
  for (size_t i = 0; i < 2; ++i) {
    cell::BusArbiterConfig config;
    config.nodeId = sim.nodes[i].id;
    config.holdMs = 120;
    config.tokenTimeoutMs = 400;
    config.claimSlotMs = 40;
    config.maxFramesPerTurn = 4;
    sim.nodes[i].arbiter.begin(config, 0);
    sim.nodes[i].arbiter.setAliveMask(0x03);
  }
}

}  // namespace

// Two users press "send" at the same instant, in opposite directions.
void testConcurrentSendersBothMessagesArriveExactlyOnce() {
  Sim sim;
  simSetup(sim, 1, 2);

  sim.nodes[0].queue.push(cell::FrameQueue::kPrioText,
                          makeDataFrame(1, 2, 100, cell::kFlagAckRequired, 0xA1), 0);
  sim.nodes[1].queue.push(cell::FrameQueue::kPrioText,
                          makeDataFrame(2, 1, 200, cell::kFlagAckRequired, 0xB2), 0);

  // Run until both messages are delivered AND both acknowledgements came back:
  // stopping at the delivery alone would leave the last ACK in flight.
  for (uint32_t step = 0; step < 2000; ++step) {
    simStep(sim);
    if (sim.nodes[0].delivered >= 1 && sim.nodes[1].delivered >= 1 &&
        sim.nodes[0].acks.pendingMessages() == 0 && sim.nodes[1].acks.pendingMessages() == 0) {
      break;
    }
  }

  TEST_ASSERT_EQUAL_UINT8(1, sim.nodes[1].delivered);
  TEST_ASSERT_EQUAL_UINT8(1, sim.nodes[0].delivered);
  TEST_ASSERT_EQUAL_UINT8(0xA1, sim.nodes[1].lastDeliveredMarker);
  TEST_ASSERT_EQUAL_UINT8(0xB2, sim.nodes[0].lastDeliveredMarker);
  TEST_ASSERT_EQUAL_UINT32(100U, sim.nodes[1].lastDeliveredSeq);
  TEST_ASSERT_EQUAL_UINT32(200U, sim.nodes[0].lastDeliveredSeq);

  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(sim.nodes[0].acks.pendingMessages()));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(sim.nodes[1].acks.pendingMessages()));
  TEST_ASSERT_EQUAL_UINT32(0U, sim.nodes[0].acks.failures());
  TEST_ASSERT_EQUAL_UINT32(0U, sim.nodes[1].acks.failures());

  // No retransmission storm: two data frames plus their two acknowledgements.
  const uint32_t totalFrames = sim.nodes[0].framesSent + sim.nodes[1].framesSent;
  TEST_ASSERT_TRUE(totalFrames <= 8U);
  TEST_ASSERT_EQUAL_UINT32(0U, sim.nodes[0].acks.retransmissions());
  TEST_ASSERT_EQUAL_UINT32(0U, sim.nodes[1].acks.retransmissions());
}

// A full egress queue in each direction: the ring must interleave both
// directions instead of draining one station first (fairness, no starvation).
void testConcurrentSendersAreFairInBothDirections() {
  Sim sim;
  simSetup(sim, 1, 2);

  const uint8_t perDirection = static_cast<uint8_t>(cell::kQueueCapacity);
  for (uint8_t i = 0; i < perDirection; ++i) {
    TEST_ASSERT_TRUE(sim.nodes[0].queue.push(
        cell::FrameQueue::kPrioText,
        makeDataFrame(1, 2, static_cast<uint16_t>(100 + i), cell::kFlagAckRequired, 0xA1), 0));
    TEST_ASSERT_TRUE(sim.nodes[1].queue.push(
        cell::FrameQueue::kPrioText,
        makeDataFrame(2, 1, static_cast<uint16_t>(200 + i), cell::kFlagAckRequired, 0xB2), 0));
  }

  for (uint32_t step = 0; step < 20000; ++step) {
    simStep(sim);
    if (sim.nodes[0].delivered >= perDirection && sim.nodes[1].delivered >= perDirection &&
        sim.nodes[0].acks.pendingMessages() == 0 && sim.nodes[1].acks.pendingMessages() == 0) {
      break;
    }
  }

  TEST_ASSERT_EQUAL_UINT8(perDirection, sim.nodes[1].delivered);
  TEST_ASSERT_EQUAL_UINT8(perDirection, sim.nodes[0].delivered);
  TEST_ASSERT_EQUAL_UINT32(0U, sim.nodes[0].acks.failures());
  TEST_ASSERT_EQUAL_UINT32(0U, sim.nodes[1].acks.failures());
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(sim.nodes[0].queue.size()));
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(sim.nodes[1].queue.size()));
}

// Both stations want to transmit but neither holds the token: the deterministic
// claim slots must recover the ring without losing a message.
void testConcurrentSendersRecoverFromALostToken() {
  Sim sim;
  simSetup(sim, 1, 2);

  sim.nodes[0].queue.push(cell::FrameQueue::kPrioText,
                          makeDataFrame(1, 2, 300, cell::kFlagAckRequired, 0xC3), 0);
  sim.nodes[1].queue.push(cell::FrameQueue::kPrioText,
                          makeDataFrame(2, 1, 400, cell::kFlagAckRequired, 0xD4), 0);

  for (uint32_t step = 0; step < 5000 && (sim.nodes[0].delivered < 1 || sim.nodes[1].delivered < 1);
       ++step) {
    simStep(sim);
  }
  TEST_ASSERT_EQUAL_UINT8(1, sim.nodes[0].delivered);
  TEST_ASSERT_EQUAL_UINT8(1, sim.nodes[1].delivered);
}

// A station that goes silent after a frame was sent must not lose it for ever:
// the retransmissions fire on real silence and the frame arrives when the peer
// is switched back on.
void testSilentPeerIsRecoveredByRetransmission() {
  Sim sim;
  simSetup(sim, 1, 2);

  sim.nodes[1].muted = true;  // the peer is off: no answer, no token, no ack
  sim.nodes[0].queue.push(cell::FrameQueue::kPrioText,
                          makeDataFrame(1, 2, 777, cell::kFlagAckRequired, 0xE5), 0);

  // The sender keeps trying while the peer stays mute.
  for (uint32_t step = 0; step < 2000 && sim.nodes[0].acks.retransmissions() < 1U; ++step) {
    simStep(sim);
  }
  TEST_ASSERT_TRUE(sim.nodes[0].acks.retransmissions() >= 1U);
  TEST_ASSERT_EQUAL_UINT8(0, sim.nodes[1].delivered);

  // The peer comes back before the attempts are exhausted: the next
  // retransmission reaches it and is acknowledged. The loop waits for the
  // acknowledgement itself, not only for the delivery.
  sim.nodes[1].muted = false;
  for (uint32_t step = 0; step < 12000; ++step) {
    simStep(sim);
    if (sim.nodes[1].delivered >= 1 && sim.nodes[0].acks.pendingMessages() == 0) {
      break;
    }
  }
  TEST_ASSERT_EQUAL_UINT8(1, sim.nodes[1].delivered);
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(sim.nodes[0].acks.pendingMessages()));
}
