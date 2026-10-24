#include "cell_core.h"

namespace cell {
namespace {

bool isBlank(const char* text) {
  return text == nullptr || text[0] == '\0';
}

void copyBounded(char* destination, size_t capacity, const char* source) {
  if (capacity == 0U) {
    return;
  }
  memset(destination, 0, capacity);
  if (source == nullptr) {
    return;
  }
  size_t length = 0;
  while (length + 1U < capacity && source[length] != '\0') {
    ++length;
  }
  memcpy(destination, source, length);
}

constexpr uint8_t stationBit(uint8_t node) {
  return static_cast<uint8_t>(1U << ((node == 0U ? 1U : node) - 1U));
}

}  // namespace

// ---------------------------------------------------------------------------
// FrameQueue
// ---------------------------------------------------------------------------
bool FrameQueue::push(Priority priority, const Frame& frame, uint32_t nowMs) {
  Slot* ring = slots_;
  size_t* ringSize = &size_;
  if (priority < kPrioText) {
    ring = controlSlots_;
    ringSize = &controlSize_;
  }
  const size_t capacity = priority < kPrioText ? kControlQueueCapacity : kQueueCapacity;
  for (size_t i = 0; i < capacity; ++i) {
    if (!ring[i].used) {
      ring[i].frame = frame;
      ring[i].priority = priority;
      ring[i].enqueuedMs = nowMs;
      ring[i].used = true;
      ++(*ringSize);
      return true;
    }
  }
  if (priority < kPrioText) {
    ++controlDropped_;
  }
  ++dropped_;
  return false;
}

bool FrameQueue::pushProtected(Priority priority, const Frame& frame, uint32_t nowMs) {
  return push(priority, frame, nowMs);
}

bool FrameQueue::pop(Frame& out, uint32_t nowMs) {
  (void)nowMs;
  // Control and receipt frames first: they are tiny, time critical and must
  // never be delayed behind a burst of user data.
  for (size_t i = 0; i < kControlQueueCapacity; ++i) {
    if (!controlSlots_[i].used) {
      continue;
    }
    size_t best = i;
    uint8_t bestPriority = static_cast<uint8_t>(controlSlots_[i].priority);
    for (size_t j = i + 1; j < kControlQueueCapacity; ++j) {
      if (!controlSlots_[j].used) {
        continue;
      }
      if (controlSlots_[j].priority < bestPriority) {
        best = j;
        bestPriority = static_cast<uint8_t>(controlSlots_[j].priority);
      }
    }
    out = controlSlots_[best].frame;
    controlSlots_[best].used = false;
    --controlSize_;
    return true;
  }

  size_t best = kQueueCapacity;
  uint8_t bestPriority = 0xFFU;
  uint32_t bestAge = 0;
  for (size_t i = 0; i < kQueueCapacity; ++i) {
    if (!slots_[i].used) {
      continue;
    }
    if (slots_[i].priority < bestPriority) {
      best = i;
      bestPriority = slots_[i].priority;
      bestAge = slots_[i].enqueuedMs;
    } else if (slots_[i].priority == bestPriority && slots_[i].enqueuedMs < bestAge) {
      best = i;
      bestAge = slots_[i].enqueuedMs;
    }
  }
  if (best == kQueueCapacity) {
    return false;
  }
  out = slots_[best].frame;
  slots_[best].used = false;
  --size_;
  return true;
}

size_t FrameQueue::pendingOf(Priority priority) const {
  size_t total = 0;
  if (priority < kPrioText) {
    for (size_t i = 0; i < kControlQueueCapacity; ++i) {
      if (controlSlots_[i].used && controlSlots_[i].priority == priority) {
        ++total;
      }
    }
    return total;
  }
  for (size_t i = 0; i < kQueueCapacity; ++i) {
    if (slots_[i].used && slots_[i].priority == priority) {
      ++total;
    }
  }
  return total;
}

void FrameQueue::promoteStarved(uint32_t nowMs, uint32_t starvationMs) {
  for (size_t i = 0; i < kQueueCapacity; ++i) {
    if (!slots_[i].used) {
      continue;
    }
    if (slots_[i].priority < kPrioText) {
      continue;
    }
    if (static_cast<uint32_t>(nowMs - slots_[i].enqueuedMs) >= starvationMs) {
      slots_[i].priority = kPrioReceipt;
    }
  }
}

// ---------------------------------------------------------------------------
// BusArbiter
// ---------------------------------------------------------------------------
void BusArbiter::begin(const BusArbiterConfig& config, uint32_t nowMs) {
  config_ = config;
  if (config_.nodeId == 0U || config_.nodeId > kMaxStationNodes) {
    config_.nodeId = 1;
  }
  if (config_.maxFramesPerTurn == 0U) {
    config_.maxFramesPerTurn = 1;
  }
  aliveMask_ = stationBit(config_.nodeId);
  hasToken_ = false;
  turnStartMs_ = nowMs;
  lastActivityMs_ = nowMs;
  framesThisTurn_ = 0;
  tokenLostEvents_ = 0;
  turnCount_ = 0;
}

void BusArbiter::setAliveMask(uint8_t mask) {
  aliveMask_ = static_cast<uint8_t>(mask | stationBit(config_.nodeId));
}

bool BusArbiter::isAlive(uint8_t node) const {
  if (node == 0U || node > kMaxStationNodes) {
    return false;
  }
  return (aliveMask_ & stationBit(node)) != 0U;
}

uint8_t BusArbiter::aliveCount() const {
  uint8_t count = 0;
  for (uint8_t node = 1; node <= kMaxStationNodes; ++node) {
    if (isAlive(node)) {
      ++count;
    }
  }
  return count;
}

void BusArbiter::noteIdlePass() {
  if (tokenHops_ < 0xFFU) {
    ++tokenHops_;
  }
}

bool BusArbiter::mayTransmit(uint32_t nowMs) const {
  if (!hasToken_) {
    return false;
  }
  if (framesThisTurn_ >= config_.maxFramesPerTurn) {
    return false;
  }
  return static_cast<uint32_t>(nowMs - turnStartMs_) < config_.holdMs;
}

bool BusArbiter::shouldHandOver(uint32_t nowMs) const {
  return hasToken_ && !mayTransmit(nowMs);
}

bool BusArbiter::shouldClaim(uint32_t nowMs) const {
  if (hasToken_) {
    return false;
  }
  const uint32_t silence = static_cast<uint32_t>(nowMs - lastActivityMs_);
  const uint32_t threshold = static_cast<uint32_t>(config_.tokenTimeoutMs) +
                             static_cast<uint32_t>(config_.nodeId) * config_.claimSlotMs;
  return silence >= threshold;
}

void BusArbiter::claim(uint32_t nowMs) {
  if (hasToken_) {
    return;
  }
  hasToken_ = true;
  turnStartMs_ = nowMs;
  framesThisTurn_ = 0;
  tokenHops_ = 0;
  ++tokenLostEvents_;
  ++turnCount_;
}

void BusArbiter::onTokenReceived(uint32_t nowMs) {
  hasToken_ = true;
  turnStartMs_ = nowMs;
  lastActivityMs_ = nowMs;
  framesThisTurn_ = 0;
  tokenHops_ = 0;
  ++turnCount_;
}

void BusArbiter::takeToken(uint32_t nowMs, uint8_t hops) {
  onTokenReceived(nowMs);
  tokenHops_ = hops;
}

void BusArbiter::noteDataFrame(uint32_t nowMs) {
  onFrameSent(nowMs);
  tokenHops_ = 0;
}

void BusArbiter::giveTokenAway(uint32_t nowMs) {
  hasToken_ = false;
  framesThisTurn_ = 0;
  if (tokenHops_ > 0U) {
    ++idleRounds_;  // a full idle round: every alive station had an empty turn
  }
  tokenHops_ = 0;
  lastActivityMs_ = nowMs;
}

void BusArbiter::onTokenSent(uint32_t nowMs) {
  hasToken_ = false;
  framesThisTurn_ = 0;
  lastActivityMs_ = nowMs;
}

void BusArbiter::onFrameSent(uint32_t nowMs) {
  lastActivityMs_ = nowMs;
  if (framesThisTurn_ < 0xFFU) {
    ++framesThisTurn_;
  }
}

void BusArbiter::onBusActivity(uint32_t nowMs) {
  lastActivityMs_ = nowMs;
}

void BusArbiter::yieldToken() {
  hasToken_ = false;
  framesThisTurn_ = 0;
}

uint8_t BusArbiter::nextNode(uint8_t from) const {
  if (from == 0U || from > kMaxStationNodes) {
    return 1U;
  }
  return static_cast<uint8_t>((from % kMaxStationNodes) + 1U);
}

uint8_t BusArbiter::nextAliveNode(uint8_t from) const {
  uint8_t candidate = nextNode(from);
  for (uint8_t step = 0; step < kMaxStationNodes; ++step) {
    if (isAlive(candidate)) {
      return candidate;
    }
    candidate = nextNode(candidate);
  }
  return config_.nodeId;
}

// ---------------------------------------------------------------------------
// AckTracker
// ---------------------------------------------------------------------------
bool AckTracker::track(const Frame& frame, uint16_t messageSeq, uint32_t nowMs, uint32_t timeoutMs) {
  if (frame.dstNode == kNodeBroadcast || frame.dstNode == 0U) {
    ++rejected_;
    return false;
  }
  Entry* entry = nullptr;
  for (size_t i = 0; i < kMaxAckMessages; ++i) {
    if (entries_[i].used && entries_[i].destination == frame.dstNode &&
        entries_[i].firstSeq == messageSeq) {
      entry = &entries_[i];
      break;
    }
  }
  if (entry == nullptr) {
    for (size_t i = 0; i < kMaxAckMessages; ++i) {
      if (!entries_[i].used) {
        entry = &entries_[i];
        break;
      }
    }
  }
  if (entry == nullptr) {
    ++rejected_;
    return false;
  }
  if (!entry->used) {
    entry->used = true;
    entry->destination = frame.dstNode;
    entry->firstSeq = messageSeq;
    entry->fragmentCount = 0;
    entry->ackedCount = 0;
    entry->attempts = 1;
    entry->sentAtMs = nowMs;
    entry->timeoutMs = timeoutMs;
    for (size_t i = 0; i < kMaxAckFragments; ++i) {
      entry->acked[i] = false;
      entry->seqs[i] = 0;
    }
  }
  if (entry->fragmentCount >= kMaxAckFragments) {
    // A message with more fragments than the tracker can follow: the remaining
    // fragments rely on the application level recovery. Counted, never silent.
    ++rejected_;
    return false;
  }
  const size_t index = entry->fragmentCount;
  entry->seqs[index] = frame.seq;
  entry->frames[index] = frame;
  entry->acked[index] = false;
  ++entry->fragmentCount;
  if (entry->attempts == 0U) {
    entry->attempts = 1;
  }
  return true;
}

bool AckTracker::onAck(uint8_t sourceNode, uint16_t seq) {
  for (size_t i = 0; i < kMaxAckMessages; ++i) {
    Entry& entry = entries_[i];
    if (!entry.used || entry.destination != sourceNode) {
      continue;
    }
    for (size_t fragment = 0; fragment < entry.fragmentCount; ++fragment) {
      if (entry.seqs[fragment] != seq || entry.acked[fragment]) {
        continue;
      }
      entry.acked[fragment] = true;
      ++entry.ackedCount;
      if (entry.ackedCount >= entry.fragmentCount) {
        entry.used = false;  // the whole message is confirmed
      }
      return true;
    }
    // The acknowledgement names another message of the same peer: keep looking
    // at the remaining entries instead of stopping on the first match.
  }
  return false;
}

AckTracker::Action AckTracker::poll(uint32_t nowMs, uint32_t lastPeerFrameMs, Frame& out) {
  for (size_t i = 0; i < kMaxAckMessages; ++i) {
    Entry& entry = entries_[i];
    if (!entry.used) {
      continue;
    }
    const uint32_t elapsed = static_cast<uint32_t>(nowMs - entry.sentAtMs);
    if (elapsed < entry.timeoutMs) {
      continue;
    }
    if (static_cast<int32_t>(lastPeerFrameMs - entry.sentAtMs) > 0) {
      // The peer emitted a frame after our send: it is alive and working on our
      // frame (or the token is circulating). Postpone instead of duplicating.
      entry.sentAtMs = nowMs;
      continue;
    }
    if (entry.attempts >= kMaxAckAttempts) {
      entry.used = false;
      ++timeouts_;
      ++failures_;
      return Action::kFailed;
    }
    size_t fragment = kMaxAckFragments;
    for (size_t candidate = 0; candidate < entry.fragmentCount; ++candidate) {
      if (!entry.acked[candidate]) {
        fragment = candidate;
        break;
      }
    }
    if (fragment == kMaxAckFragments) {
      entry.used = false;
      continue;
    }
    out = entry.frames[fragment];
    out.flags = static_cast<uint8_t>(out.flags | kFlagRetransmit);
    ++entry.attempts;
    entry.sentAtMs = nowMs;
    if (entry.timeoutMs < 4000U) {
      entry.timeoutMs *= 2U;  // bounded exponential backoff
    }
    ++retransmissions_;
    return Action::kRetransmit;
  }
  return Action::kIdle;
}

void AckTracker::clear() {
  for (size_t i = 0; i < kMaxAckMessages; ++i) {
    entries_[i].used = false;
  }
}

size_t AckTracker::pendingMessages() const {
  size_t total = 0;
  for (size_t i = 0; i < kMaxAckMessages; ++i) {
    if (entries_[i].used) {
      ++total;
    }
  }
  return total;
}

size_t AckTracker::pendingFragments() const {
  size_t total = 0;
  for (size_t i = 0; i < kMaxAckMessages; ++i) {
    if (!entries_[i].used) {
      continue;
    }
    for (size_t fragment = 0; fragment < entries_[i].fragmentCount; ++fragment) {
      if (!entries_[i].acked[fragment]) {
        ++total;
      }
    }
  }
  return total;
}

// ---------------------------------------------------------------------------
// RoutingTable
// ---------------------------------------------------------------------------
RouteEntry* RoutingTable::findMutable(const char* userId) {
  if (isBlank(userId)) {
    return nullptr;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (strncmp(entries_[i].userId, userId, kMaxUserIdLength) == 0) {
      return &entries_[i];
    }
  }
  return nullptr;
}

RouteEntry* RoutingTable::allocate() {
  for (size_t i = 0; i < kMaxRouteEntries; ++i) {
    if (entries_[i].userId[0] == '\0') {
      if (i >= count_) {
        count_ = i + 1U;
      }
      return &entries_[i];
    }
  }
  return nullptr;
}

void RoutingTable::removeAt(size_t index) {
  if (index >= count_) {
    return;
  }
  for (size_t i = index; i + 1U < count_; ++i) {
    entries_[i] = entries_[i + 1U];
  }
  --count_;
  memset(&entries_[count_], 0, sizeof(RouteEntry));
}

bool RoutingTable::upsert(const char* userId,
                          const char* displayName,
                          uint8_t station,
                          uint32_t nowMs) {
  if (isBlank(userId) || station == 0U || station > kMaxStationNodes) {
    return false;
  }
  RouteEntry* entry = findMutable(userId);
  if (entry == nullptr) {
    entry = allocate();
  }
  if (entry == nullptr) {
    return false;
  }
  copyBounded(entry->userId, sizeof(entry->userId), userId);
  copyBounded(entry->displayName, sizeof(entry->displayName),
              isBlank(displayName) ? userId : displayName);
  entry->station = station;
  entry->lastSeenMs = nowMs;
  entry->online = true;
  return true;
}

bool RoutingTable::touch(const char* userId, uint32_t nowMs) {
  RouteEntry* entry = findMutable(userId);
  if (entry == nullptr) {
    return false;
  }
  entry->lastSeenMs = nowMs;
  entry->online = true;
  return true;
}

bool RoutingTable::remove(const char* userId) {
  for (size_t i = 0; i < count_; ++i) {
    if (strncmp(entries_[i].userId, userId, kMaxUserIdLength) == 0) {
      removeAt(i);
      return true;
    }
  }
  return false;
}

bool RoutingTable::setOnline(const char* userId, bool online) {
  RouteEntry* entry = findMutable(userId);
  if (entry == nullptr) {
    return false;
  }
  entry->online = online;
  return true;
}

const RouteEntry* RoutingTable::find(const char* userId) const {
  if (isBlank(userId)) {
    return nullptr;
  }
  for (size_t i = 0; i < count_; ++i) {
    if (strncmp(entries_[i].userId, userId, kMaxUserIdLength) == 0) {
      return &entries_[i];
    }
  }
  return nullptr;
}

void RoutingTable::expireLocal(uint8_t ownStation, uint32_t nowMs, uint32_t staleMs) {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].station != ownStation || !entries_[i].online) {
      continue;
    }
    if (static_cast<uint32_t>(nowMs - entries_[i].lastSeenMs) > staleMs) {
      entries_[i].online = false;
    }
  }
}

void RoutingTable::markStationOffline(uint8_t station) {
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].station == station) {
      entries_[i].online = false;
    }
  }
}

size_t RoutingTable::countOnline(uint8_t station) const {
  size_t total = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].station == station && entries_[i].online) {
      ++total;
    }
  }
  return total;
}

size_t RoutingTable::serializeStationState(uint8_t station,
                                           uint8_t* out,
                                           size_t capacity) const {
  if (out == nullptr || capacity < 1U) {
    return 0U;
  }
  size_t offset = 0;
  uint8_t listed = 0;
  out[offset++] = 0U;  // count placeholder
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].station != station || !entries_[i].online) {
      continue;
    }
    const size_t idLength = strnlen(entries_[i].userId, kMaxUserIdLength);
    const size_t nameLength = strnlen(entries_[i].displayName, kMaxDisplayNameLength);
    const size_t needed = 1U + idLength + 1U + nameLength;
    if (offset + needed > capacity) {
      break;
    }
    out[offset++] = static_cast<uint8_t>(idLength);
    memcpy(out + offset, entries_[i].userId, idLength);
    offset += idLength;
    out[offset++] = static_cast<uint8_t>(nameLength);
    memcpy(out + offset, entries_[i].displayName, nameLength);
    offset += nameLength;
    ++listed;
  }
  out[0] = listed;
  return offset;
}

bool RoutingTable::applyStationState(uint8_t station,
                                     const uint8_t* data,
                                     size_t length,
                                     uint32_t nowMs) {
  if (station == 0U || station > kMaxStationNodes || data == nullptr || length < 1U) {
    return false;
  }

  // Phase 1: the whole list is validated before the table is touched. A
  // truncated or corrupted announcement therefore cannot leave a half updated
  // routing state (some users offline, some others already moved).
  const uint8_t remoteCount = data[0];
  if (remoteCount > kMaxRouteEntries) {
    return false;
  }
  struct RemoteEntry {
    char userId[kMaxUserIdLength];
    char displayName[kMaxDisplayNameLength];
  };
  RemoteEntry parsed[kMaxRouteEntries];
  size_t offset = 1U;
  for (uint8_t n = 0; n < remoteCount; ++n) {
    if (offset >= length) {
      return false;
    }
    const uint8_t idLength = data[offset++];
    if (idLength == 0U || idLength > kMaxUserIdLength - 1U || offset + idLength > length) {
      return false;
    }
    memset(parsed[n].userId, 0, sizeof(parsed[n].userId));
    memcpy(parsed[n].userId, data + offset, idLength);
    offset += idLength;

    if (offset >= length) {
      return false;
    }
    const uint8_t nameLength = data[offset++];
    if (nameLength > kMaxDisplayNameLength - 1U || offset + nameLength > length) {
      return false;
    }
    memset(parsed[n].displayName, 0, sizeof(parsed[n].displayName));
    memcpy(parsed[n].displayName, data + offset, nameLength);
    offset += nameLength;
  }

  // Phase 2: the received list is authoritative for that station. Every known
  // user of the station that is absent from the new list goes offline.
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].station != station) {
      continue;
    }
    bool present = false;
    for (uint8_t n = 0; n < remoteCount && !present; ++n) {
      if (strcmp(entries_[i].userId, parsed[n].userId) == 0) {
        present = true;
      }
    }
    if (present) {
      entries_[i].lastSeenMs = nowMs;
    } else {
      entries_[i].online = false;
    }
  }

  for (uint8_t n = 0; n < remoteCount; ++n) {
    upsert(parsed[n].userId, parsed[n].displayName, station, nowMs);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Application envelope
// ---------------------------------------------------------------------------
size_t buildAppEnvelope(const char* source,
                        const char* destination,
                        const uint8_t* body,
                        size_t bodyLength,
                        uint8_t* out,
                        size_t capacity) {
  if (out == nullptr || source == nullptr || destination == nullptr) {
    return 0U;
  }
  const size_t srcLength = strnlen(source, kMaxUserIdLength - 1U);
  const size_t dstLength = strnlen(destination, kMaxUserIdLength - 1U);
  if (srcLength == 0U || dstLength == 0U) {
    return 0U;
  }
  const size_t needed = 1U + dstLength + 1U + srcLength + bodyLength;
  if (needed > capacity) {
    return 0U;
  }
  size_t offset = 0;
  out[offset++] = static_cast<uint8_t>(dstLength);
  memcpy(out + offset, destination, dstLength);
  offset += dstLength;
  out[offset++] = static_cast<uint8_t>(srcLength);
  memcpy(out + offset, source, srcLength);
  offset += srcLength;
  if (bodyLength > 0U && body != nullptr) {
    memcpy(out + offset, body, bodyLength);
    offset += bodyLength;
  }
  return offset;
}

bool parseAppEnvelope(const uint8_t* payload, size_t length, AppEnvelope& out) {
  if (payload == nullptr || length < 2U) {
    return false;
  }
  size_t offset = 0;
  const uint8_t dstLength = payload[offset++];
  if (dstLength == 0U || dstLength >= kMaxUserIdLength || offset + dstLength > length) {
    return false;
  }
  memset(out.destination, 0, sizeof(out.destination));
  memcpy(out.destination, payload + offset, dstLength);
  offset += dstLength;

  if (offset >= length) {
    return false;
  }
  const uint8_t srcLength = payload[offset++];
  if (srcLength == 0U || srcLength >= kMaxUserIdLength || offset + srcLength > length) {
    return false;
  }
  memset(out.source, 0, sizeof(out.source));
  memcpy(out.source, payload + offset, srcLength);
  offset += srcLength;

  out.body = payload + offset;
  out.bodyLength = length - offset;
  return true;
}

// ---------------------------------------------------------------------------
// FragmentAssembler
// ---------------------------------------------------------------------------
void FragmentAssembler::reset() {
  for (size_t i = 0; i < kReassemblyContexts; ++i) {
    contexts_[i].used = false;
    contexts_[i].receivedMask = 0;
    contexts_[i].receivedCount = 0;
  }
  lastPayload_ = nullptr;
  lastLength_ = 0;
}

void FragmentAssembler::expire(uint32_t nowMs, uint32_t timeoutMs) {
  for (size_t i = 0; i < kReassemblyContexts; ++i) {
    if (!contexts_[i].used) {
      continue;
    }
    if (static_cast<uint32_t>(nowMs - contexts_[i].lastMs) > timeoutMs) {
      contexts_[i].used = false;
      ++rejected_;
    }
  }
}

FragmentAssembler::Status FragmentAssembler::accept(const Frame& fragment,
                                                    uint32_t nowMs,
                                                    uint8_t* out,
                                                    size_t capacity,
                                                    size_t& outLength) {
  // A rejected fragment must never leave a stale view behind.
  lastPayload_ = nullptr;
  lastLength_ = 0;
  if ((fragment.flags & kFlagFragmented) == 0U || fragment.payloadLength < kFragmentOverhead) {
    ++rejected_;
    return Status::kError;
  }
  const uint8_t index = fragment.payload[0];
  const uint8_t count = fragment.payload[1];
  const uint16_t messageSeq = static_cast<uint16_t>(fragment.payload[2]) |
                              (static_cast<uint16_t>(fragment.payload[3]) << 8);
  if (count == 0U || count > kMaxFragments || index >= count) {
    ++rejected_;
    return Status::kError;
  }
  const size_t chunkLength = fragment.payloadLength - kFragmentOverhead;
  if (chunkLength == 0U || chunkLength > kFragmentBodySize) {
    ++rejected_;
    return Status::kError;
  }

  Context* context = nullptr;
  for (size_t i = 0; i < kReassemblyContexts; ++i) {
    if (contexts_[i].used && contexts_[i].srcNode == fragment.srcNode &&
        contexts_[i].type == fragment.type && contexts_[i].messageSeq == messageSeq) {
      context = &contexts_[i];
      break;
    }
  }
  if (context == nullptr) {
    for (size_t i = 0; i < kReassemblyContexts; ++i) {
      if (!contexts_[i].used) {
        context = &contexts_[i];
        break;
      }
    }
  }
  if (context == nullptr) {
    // Recycle the least recently used context.
    context = &contexts_[0];
    for (size_t i = 1; i < kReassemblyContexts; ++i) {
      if (static_cast<int32_t>(contexts_[i].lastMs - context->lastMs) < 0) {
        context = &contexts_[i];
      }
    }
    ++rejected_;
  }
  const bool sameMessage = context->used && context->srcNode == fragment.srcNode &&
                           context->type == fragment.type && context->messageSeq == messageSeq;
  if (!sameMessage) {
    context->used = true;
    context->srcNode = fragment.srcNode;
    context->type = fragment.type;
    context->messageSeq = messageSeq;
    context->fragmentCount = count;
    context->receivedCount = 0;
    context->receivedMask = 0;
    memset(context->lengths, 0, sizeof(context->lengths));
  }
  if (context->fragmentCount != count) {
    ++rejected_;
    return Status::kError;
  }
  context->lastMs = nowMs;

  const uint16_t bit = static_cast<uint16_t>(1U << index);
  if ((context->receivedMask & bit) != 0U) {
    return Status::kIncomplete;  // duplicate fragment, ignored
  }

  memcpy(context->data + static_cast<size_t>(index) * kFragmentBodySize,
         fragment.payload + kFragmentOverhead, chunkLength);
  context->lengths[index] = static_cast<uint16_t>(chunkLength);
  context->receivedMask |= bit;
  ++context->receivedCount;

  if (context->receivedCount < count) {
    return Status::kIncomplete;
  }

  size_t total = 0;
  for (uint8_t i = 0; i < count; ++i) {
    total += context->lengths[i];
  }
  if (total > capacity) {
    context->used = false;
    ++rejected_;
    return Status::kError;
  }
  size_t written = 0;
  for (uint8_t i = 0; i < count; ++i) {
    memcpy(out + written, context->data + static_cast<size_t>(i) * kFragmentBodySize,
           context->lengths[i]);
    written += context->lengths[i];
  }
  // Publish a zero copy view of the message before releasing the context: the
  // caller delivers it synchronously, without copying it into a small Frame.
  lastPayload_ = context->data;
  lastLength_ = written;
  context->used = false;
  ++completed_;
  outLength = written;
  return Status::kComplete;
}

}  // namespace cell
