#ifndef CELL_CORE_H
#define CELL_CORE_H

// Platform neutral building blocks of the inter-cell transport:
//  - FrameQueue     : bounded, priority aware egress queue with anti starvation
//  - BusArbiter     : deterministic token based access to the shared RS485 bus
//  - RoutingTable   : user -> station presence table shared over the bus
//
// No Arduino dependency, so the same code runs on the ESP32 and in env:native
// unit tests.

#include <cstddef>
#include <cstdint>

#include "cell_protocol.h"

namespace cell {

constexpr size_t kQueueCapacity = 8;
// Control and receipt traffic has its own ring: an acknowledgement can never
// take the place of a user message, and a burst of user messages can never make
// the bus acknowledgements disappear. Both directions of the protection are
// therefore explicit instead of one sacrificing the other.
constexpr size_t kControlQueueCapacity = 6;
constexpr size_t kMaxRouteEntries = 16;
constexpr size_t kMaxUserIdLength = 24;
constexpr size_t kMaxDisplayNameLength = 32;

// ---------------------------------------------------------------------------
// Egress queue
// ---------------------------------------------------------------------------
class FrameQueue {
 public:
  enum Priority : uint8_t {
    kPrioControl = 0,   // token, ping, route state, recovery
    kPrioReceipt = 1,   // bus acks, read receipts, voice feedback
    kPrioText = 2,      // text messages
    kPrioAudio = 3,     // voice blocks
    kPrioCount = 4,
  };

  // Routes the frame to the control ring (priority < kPrioText) or to the
  // application ring. Strict: returns false when the target ring is full, which
  // the caller reports as an explicit back pressure.
  bool push(Priority priority, const Frame& frame, uint32_t nowMs);

  // Same as push(), but only meaningful for control/receipt priorities: it is
  // the call site contract "this frame must not be lost". Application frames
  // keep using push().
  bool pushProtected(Priority priority, const Frame& frame, uint32_t nowMs);

  // Control frames are served first (they are tiny and time critical), then the
  // application frames by priority, oldest first within a priority.
  bool pop(Frame& out, uint32_t nowMs);
  bool empty() const { return size_ == 0 && controlSize_ == 0; }
  size_t size() const { return size_; }
  size_t controlSize() const { return controlSize_; }
  size_t freeSlots() const { return kQueueCapacity - size_; }
  uint32_t droppedCount() const { return dropped_; }
  uint32_t controlDroppedCount() const { return controlDropped_; }
  size_t pendingOf(Priority priority) const;

  // Promotes audio entries older than starvationMs so that a steady text flow
  // can never starve the voice traffic forever.
  void promoteStarved(uint32_t nowMs, uint32_t starvationMs);

 private:
  struct Slot {
    Frame frame;
    Priority priority = kPrioText;
    uint32_t enqueuedMs = 0;
    bool used = false;
  };
  Slot slots_[kQueueCapacity];
  Slot controlSlots_[kControlQueueCapacity];
  size_t size_ = 0;
  size_t controlSize_ = 0;
  uint32_t dropped_ = 0;
  uint32_t controlDropped_ = 0;
};

// ---------------------------------------------------------------------------
// Bus arbitration
// ---------------------------------------------------------------------------
// The bus is half duplex and shared by three nodes. A token circulates between
// the stations that are currently considered alive:
//
//   * only the token holder may transmit;
//   * a holder transmits at most maxFramesPerTurn frames or holdMs, then hands
//     the token to the next alive station;
//   * if no traffic is seen for tokenTimeoutMs (+ nodeId * claimSlotMs), the
//     lowest node id with pending data claims the token, which recovers from a
//     lost token or a silent / rebooting station.
//
// The per-node claim slot makes the recovery deterministic: two stations never
// claim in the same time slot, and a collision still resolves on the next
// timeout because the slots differ by node id.
struct BusArbiterConfig {
  uint8_t nodeId = 1;
  uint16_t holdMs = 120;
  uint16_t tokenTimeoutMs = 400;
  uint16_t claimSlotMs = 40;
  uint8_t maxFramesPerTurn = 4;
};

class BusArbiter {
 public:
  void begin(const BusArbiterConfig& config, uint32_t nowMs);

  void setAliveMask(uint8_t mask);
  uint8_t aliveMask() const { return aliveMask_; }
  bool isAlive(uint8_t node) const;

  bool hasToken() const { return hasToken_; }
  bool mayTransmit(uint32_t nowMs) const;
  bool shouldHandOver(uint32_t nowMs) const;
  bool shouldClaim(uint32_t nowMs) const;

  void claim(uint32_t nowMs);
  void onTokenReceived(uint32_t nowMs);
  void onTokenSent(uint32_t nowMs);
  void onFrameSent(uint32_t nowMs);
  void onBusActivity(uint32_t nowMs);
  void yieldToken();

  // Token forwarding used by the mesh node:
  //   takeToken      : we just received the token (bare TOKEN frame or attached
  //                    to the last data frame of a peer);
  //   noteDataFrame  : a data frame was really sent, the ring is alive (hops=0);
  //   giveTokenAway  : we drop the token, the bus becomes silent.
  void takeToken(uint32_t nowMs, uint8_t hops);
  void noteDataFrame(uint32_t nowMs);
  void giveTokenAway(uint32_t nowMs);
  uint8_t tokenHops() const { return tokenHops_; }

  // Idle round tracking. A station that holds the token with nothing to send
  // passes it to the next alive station instead of dropping it: the token only
  // leaves the ring once every alive station had one turn with an empty queue.
  // Without this, every message paid a full claim timeout (400 ms + slot) just
  // because the local queue happened to be empty at that instant.
  // `tokenHops_` doubles as the idle pass counter (it counts the stations that
  // held the token without sending a data frame).
  void noteIdlePass();
  uint8_t idlePasses() const { return tokenHops_; }
  void resetIdlePasses() { tokenHops_ = 0; }
  uint32_t idleRounds() const { return idleRounds_; }
  uint8_t aliveCount() const;

  uint8_t nextAliveNode(uint8_t from) const;
  uint8_t nextNode(uint8_t from) const;
  uint8_t maxFramesPerTurn() const { return config_.maxFramesPerTurn; }
  uint16_t tokenTimeoutMs() const { return config_.tokenTimeoutMs; }
  uint16_t holdMs() const { return config_.holdMs; }
  uint8_t framesThisTurn() const { return framesThisTurn_; }

  uint32_t tokenLostEvents() const { return tokenLostEvents_; }
  uint32_t turnCount() const { return turnCount_; }
  uint32_t lastActivityMs() const { return lastActivityMs_; }
  uint8_t nodeId() const { return config_.nodeId; }

 private:
  BusArbiterConfig config_;
  uint8_t aliveMask_ = 0;
  bool hasToken_ = false;
  uint32_t turnStartMs_ = 0;
  uint32_t lastActivityMs_ = 0;
  uint8_t framesThisTurn_ = 0;
  uint8_t tokenHops_ = 0;
  uint32_t idleRounds_ = 0;
  uint32_t tokenLostEvents_ = 0;
  uint32_t turnCount_ = 0;
};

// ---------------------------------------------------------------------------
// Transport level acknowledgement tracking (fragment aware)
// ---------------------------------------------------------------------------
// A logical message may be split in several bus fragments. Every fragment is
// acknowledged individually by the receiving station, so the tracker keys its
// entries by (destination, first sequence) and keeps one bit per fragment:
//   * an acknowledgement clears exactly the fragment it names, never another
//     message or another fragment;
//   * only the fragments still unacknowledged are retransmitted (selective
//     retransmission), never the whole message;
//   * a retransmission is only scheduled after a real bus silence. While the
//     bus shows activity the peer is demonstrably working on our frame, so the
//     deadline is postponed instead of sending a useless duplicate. This is
//     what removed the retransmission storms caused by a fixed 300 ms timeout
//     against the 400 ms token claim window.
constexpr size_t kMaxAckMessages = 3;
constexpr size_t kMaxAckFragments = 4;
constexpr uint8_t kMaxAckAttempts = 4;

class AckTracker {
 public:
  enum class Action : uint8_t {
    kIdle,        // nothing to do
    kRetransmit,  // out holds the fragment to send again
    kFailed,      // one message exhausted its attempts, it was forgotten
  };

  // Registers a frame that requires an acknowledgement. `messageSeq` is the
  // sequence of the first fragment of the logical message (equal to frame.seq
  // for a single frame message). Returns false when the tracker is full.
  bool track(const Frame& frame, uint16_t messageSeq, uint32_t nowMs, uint32_t timeoutMs);

  // Clears the fragment named by (sourceNode, sequence).
  bool onAck(uint8_t sourceNode, uint16_t seq);

  // Next retransmission decision. `lastPeerFrameMs` is the timestamp of the last
  // frame received from the bus (0 when none): a peer frame received after our
  // own send proves the peer is alive and working, so the deadline of that entry
  // is postponed instead of sending a duplicate. Our own transmission is
  // deliberately not counted, otherwise nothing would ever be retransmitted.
  Action poll(uint32_t nowMs, uint32_t lastPeerFrameMs, Frame& out);

  void clear();
  size_t pendingMessages() const;
  size_t pendingFragments() const;
  uint32_t timeouts() const { return timeouts_; }
  uint32_t retransmissions() const { return retransmissions_; }
  uint32_t failures() const { return failures_; }
  uint32_t rejected() const { return rejected_; }

 private:
  struct Entry {
    bool used = false;
    uint8_t destination = 0;
    uint16_t firstSeq = 0;
    uint8_t fragmentCount = 0;
    uint8_t ackedCount = 0;
    uint8_t attempts = 0;
    uint32_t sentAtMs = 0;
    uint32_t timeoutMs = 0;
    uint16_t seqs[kMaxAckFragments] = {0};
    bool acked[kMaxAckFragments] = {false};
    Frame frames[kMaxAckFragments];
  };

  Entry entries_[kMaxAckMessages];
  uint32_t timeouts_ = 0;
  uint32_t retransmissions_ = 0;
  uint32_t failures_ = 0;
  uint32_t rejected_ = 0;
};

// ---------------------------------------------------------------------------
// Presence / routing table
// ---------------------------------------------------------------------------
struct RouteEntry {
  char userId[kMaxUserIdLength] = {0};
  char displayName[kMaxDisplayNameLength] = {0};
  uint8_t station = 0;
  uint32_t lastSeenMs = 0;
  bool online = false;
};

class RoutingTable {
 public:
  bool upsert(const char* userId,
              const char* displayName,
              uint8_t station,
              uint32_t nowMs);
  bool touch(const char* userId, uint32_t nowMs);
  bool remove(const char* userId);
  bool setOnline(const char* userId, bool online);
  const RouteEntry* find(const char* userId) const;

  size_t entries() const { return count_; }
  const RouteEntry& at(size_t index) const { return entries_[index]; }

  // Local users that did not refresh their presence in time are set offline.
  void expireLocal(uint8_t ownStation, uint32_t nowMs, uint32_t staleMs);
  void markStationOffline(uint8_t station);
  size_t countOnline(uint8_t station) const;

  // Binary presence list for one station:
  //   [count:1] then count * ([idLen:1][id][nameLen:1][name])
  size_t serializeStationState(uint8_t station, uint8_t* out, size_t capacity) const;
  // Replaces the known presence of `station` with the received list.
  bool applyStationState(uint8_t station,
                         const uint8_t* data,
                         size_t length,
                         uint32_t nowMs);

 private:
  RouteEntry* findMutable(const char* userId);
  RouteEntry* allocate();
  void removeAt(size_t index);

  RouteEntry entries_[kMaxRouteEntries];
  size_t count_ = 0;
};

// ---------------------------------------------------------------------------
// Application envelope carried inside a bus frame payload
// ---------------------------------------------------------------------------
// [dstLen:1][dst][srcLen:1][src][body...]
// The body is opaque for the firmware: JSON for text/receipts, a binary voice
// block for audio. Keeping the two user ids inside the transport payload means
// the receiving station can deliver to the right phone socket without parsing
// application JSON.
struct AppEnvelope {
  char destination[kMaxUserIdLength] = {0};
  char source[kMaxUserIdLength] = {0};
  const uint8_t* body = nullptr;
  size_t bodyLength = 0;
};

size_t buildAppEnvelope(const char* source,
                        const char* destination,
                        const uint8_t* body,
                        size_t bodyLength,
                        uint8_t* out,
                        size_t capacity);

bool parseAppEnvelope(const uint8_t* payload, size_t length, AppEnvelope& out);

// ---------------------------------------------------------------------------
// Application payload fragmentation
// ---------------------------------------------------------------------------
// Fragmented payload layout (kFlagFragmented set):
//   [index:1][count:1][messageSeq:2][chunk...]
// `messageSeq` is the bus sequence of the first fragment and identifies the
// whole message.
//
// 12 fragments x 476 bytes = 5712 bytes of reassembled payload. This is what
// allows a single application envelope (JSON text, receipt, voice block) to be
// much larger than one bus frame; longer texts are additionally split by the
// Android application itself, so the bus only ever carries bounded chunks.
constexpr uint8_t kMaxFragments = 12;
constexpr size_t kFragmentOverhead = 4;
constexpr size_t kFragmentBodySize = kMaxBusPayload - kFragmentOverhead;
constexpr size_t kMaxReassembledPayload = kMaxFragments * kFragmentBodySize;
constexpr size_t kReassemblyContexts = 2;

class FragmentAssembler {
 public:
  enum class Status : uint8_t { kIncomplete, kComplete, kError };

  // Feeds one fragment. When Status::kComplete is returned, the reassembled
  // payload is available through lastPayload()/lastLength().
  Status accept(const Frame& fragment,
                uint32_t nowMs,
                uint8_t* out,
                size_t capacity,
                size_t& outLength);
  void expire(uint32_t nowMs, uint32_t timeoutMs);
  void reset();

  // Zero copy view of the last completed message. The pointer stays valid until
  // the next accept() call (the context is then reused), which is enough because
  // the message is delivered synchronously.
  //
  // This view is what removed a real stack overflow: the previous delivery path
  // copied the assembled payload into Frame::payload, a 480 byte buffer, while a
  // reassembled message can be up to kMaxReassembledPayload (5712) bytes.
  const uint8_t* lastPayload() const { return lastPayload_; }
  size_t lastLength() const { return lastLength_; }

  uint32_t completedCount() const { return completed_; }
  uint32_t rejectedCount() const { return rejected_; }

 private:
  struct Context {
    bool used = false;
    uint8_t srcNode = 0;
    uint8_t type = 0;
    uint16_t messageSeq = 0;
    uint8_t fragmentCount = 0;
    uint8_t receivedCount = 0;
    uint16_t receivedMask = 0;
    uint16_t lengths[kMaxFragments] = {0};
    uint32_t lastMs = 0;
    uint8_t data[kMaxReassembledPayload] = {0};
  };

  Context contexts_[kReassemblyContexts];
  const uint8_t* lastPayload_ = nullptr;
  size_t lastLength_ = 0;
  uint32_t completed_ = 0;
  uint32_t rejected_ = 0;
};

}  // namespace cell

#endif  // CELL_CORE_H
