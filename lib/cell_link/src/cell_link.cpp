#include "cell_link.h"

#include <cstring>

namespace cell {
namespace {

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

bool isAlphanumeric(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
         (value >= 'A' && value <= 'Z');
}

// True when the fixed width field holds a terminated, non empty identifier.
bool fieldIsValid(const uint8_t* field, size_t size) {
  if (field == nullptr || size == 0U || field[0] == 0U) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    if (field[i] == 0U) {
      return true;
    }
  }
  return false;  // no terminator inside the field: malformed
}

uint32_t readLe32(const uint8_t* data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) |
         (static_cast<uint32_t>(data[offset + 1U]) << 8) |
         (static_cast<uint32_t>(data[offset + 2U]) << 16) |
         (static_cast<uint32_t>(data[offset + 3U]) << 24);
}

void writeLe32(uint8_t* data, size_t offset, uint32_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFFU);
  data[offset + 1U] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  data[offset + 2U] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  data[offset + 3U] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

uint8_t nodeBit(uint8_t node) {
  if (node == 0U || node > 8U) {
    return 0U;
  }
  return static_cast<uint8_t>(1U << (node - 1U));
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
uint16_t linkDeclaredLength(const uint8_t* header, size_t length) {
  if (header == nullptr || length < kLinkHeaderSize) {
    return 0U;
  }
  if (header[0] != kLinkMagic || header[1] != kLinkVersion) {
    return 0U;
  }
  const uint32_t declared = readLe32(header, 8U);
  if (declared > kLinkMaxPayload) {
    return 0U;
  }
  return static_cast<uint16_t>(declared);
}

size_t encodeLinkFrame(const LinkFrame& frame, uint8_t* out, size_t capacity) {
  if (out == nullptr || frame.payloadLength > kLinkMaxPayload) {
    return 0U;
  }
  const size_t total = kLinkHeaderSize + frame.payloadLength;
  if (capacity < total) {
    return 0U;
  }
  out[0] = kLinkMagic;
  out[1] = kLinkVersion;
  out[2] = frame.type;
  out[3] = frame.flags;
  out[4] = frame.srcNode;
  out[5] = frame.dstNode;
  out[6] = frame.hops;
  out[7] = 0U;  // reserved
  writeLe32(out, 8U, frame.payloadLength);
  if (frame.payloadLength > 0U) {
    memcpy(out + kLinkHeaderSize, frame.payload, frame.payloadLength);
  }
  return total;
}

LinkDecodeResult decodeLinkFrame(const uint8_t* data, size_t length, LinkFrame& out) {
  if (data == nullptr || length < kLinkHeaderSize) {
    return LinkDecodeResult::kInvalid;
  }
  const uint16_t declared = linkDeclaredLength(data, length);
  if (data[0] != kLinkMagic || data[1] != kLinkVersion) {
    return LinkDecodeResult::kInvalid;
  }
  // The declared length must match the buffer exactly: a frame claiming more
  // bytes than it carries is rejected instead of being read out of bounds.
  if (length != static_cast<size_t>(kLinkHeaderSize) + declared) {
    return LinkDecodeResult::kInvalid;
  }
  if (data[4] == 0U || data[4] > 8U || data[5] > 8U) {
    return LinkDecodeResult::kInvalid;
  }
  out.type = data[2];
  out.flags = data[3];
  out.srcNode = data[4];
  out.dstNode = data[5];
  out.hops = data[6];
  out.payloadLength = declared;
  if (declared > 0U) {
    memcpy(out.payload, data + kLinkHeaderSize, declared);
  }
  return LinkDecodeResult::kOk;
}

// ---------------------------------------------------------------------------
// Call signalling payload
// ---------------------------------------------------------------------------
size_t buildLinkCallSignal(const LinkCallSignal& signal, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kLinkCallSignalSize) {
    return 0U;
  }
  for (size_t i = 0; i < kLinkCallIdLength; ++i) {
    if (!isAlphanumeric(signal.callId[i])) {
      return 0U;
    }
  }
  if (signal.callId[kLinkCallIdLength] != '\0') {
    return 0U;
  }
  if (signal.action < kSignalInvite || signal.action > kSignalEnded) {
    return 0U;
  }
  if (signal.from[0] == '\0' || signal.to[0] == '\0') {
    return 0U;
  }
  memset(out, 0, kLinkCallSignalSize);
  memcpy(out, signal.callId, kLinkCallIdLength);
  out[8] = signal.action;
  out[9] = signal.reason;
  copyBounded(reinterpret_cast<char*>(out) + 10U, kLinkUserLength, signal.from);
  copyBounded(reinterpret_cast<char*>(out) + 34U, kLinkUserLength, signal.to);
  return kLinkCallSignalSize;
}

bool parseLinkCallSignal(const uint8_t* data, size_t length, LinkCallSignal& out) {
  if (data == nullptr || length != kLinkCallSignalSize) {
    return false;
  }
  char callId[kLinkCallIdLength + 1] = {0};
  memcpy(callId, data, kLinkCallIdLength);
  for (size_t i = 0; i < kLinkCallIdLength; ++i) {
    if (!isAlphanumeric(callId[i])) {
      return false;
    }
  }
  const uint8_t action = data[8];
  if (action < kSignalInvite || action > kSignalEnded) {
    return false;
  }
  if (!fieldIsValid(data + 10U, kLinkUserLength) || !fieldIsValid(data + 34U, kLinkUserLength)) {
    return false;
  }
  copyBounded(out.callId, sizeof(out.callId), callId);
  out.action = action;
  out.reason = data[9];
  copyBounded(out.from, sizeof(out.from), reinterpret_cast<const char*>(data) + 10U);
  copyBounded(out.to, sizeof(out.to), reinterpret_cast<const char*>(data) + 34U);
  return out.from[0] != '\0' && out.to[0] != '\0';
}

// ---------------------------------------------------------------------------
// Authentication handshake payload
// ---------------------------------------------------------------------------
size_t buildLinkHello(uint8_t nodeId, const char* token, uint8_t* out, size_t capacity) {
  if (out == nullptr || capacity < kLinkHelloPayloadSize || nodeId == 0U || nodeId > 8U) {
    return 0U;
  }
  memset(out, 0, kLinkHelloPayloadSize);
  out[0] = nodeId;
  copyBounded(reinterpret_cast<char*>(out) + 1U, kLinkTokenLength, token);
  return kLinkHelloPayloadSize;
}

bool parseLinkHello(const uint8_t* data,
                    size_t length,
                    uint8_t& nodeIdOut,
                    char* tokenOut,
                    size_t tokenCapacity) {
  nodeIdOut = 0U;
  if (tokenOut != nullptr && tokenCapacity > 0U) {
    tokenOut[0] = '\0';
  }
  if (data == nullptr || length != kLinkHelloPayloadSize || tokenOut == nullptr ||
      tokenCapacity < kLinkTokenLength) {
    return false;
  }
  const uint8_t nodeId = data[0];
  if (nodeId == 0U || nodeId > 8U) {
    return false;
  }
  if (!fieldIsValid(data + 1U, kLinkTokenLength)) {
    return false;
  }
  nodeIdOut = nodeId;
  copyBounded(tokenOut, tokenCapacity, reinterpret_cast<const char*>(data) + 1U);
  return tokenOut[0] != '\0';
}

// ---------------------------------------------------------------------------
// Stream reassembly
// ---------------------------------------------------------------------------
void LinkStreamAssembler::reset() {
  length_ = 0U;
  expected_ = 0U;
}

LinkStreamAssembler::Status LinkStreamAssembler::push(uint8_t byte, LinkFrame& out) {
  if (length_ >= sizeof(buffer_)) {
    // Cannot happen with a correct reader, but never overflow the buffer.
    ++errors_;
    reset();
  }
  buffer_[length_++] = byte;

  if (length_ < kLinkHeaderSize) {
    return Status::kIdle;
  }
  if (expected_ == 0U) {
    if (buffer_[0] != kLinkMagic || buffer_[1] != kLinkVersion) {
      // Not a header: drop this byte and try to resynchronise on the next one.
      ++errors_;
      reset();
      return Status::kError;
    }
    const uint32_t declared = readLe32(buffer_, 8U);
    if (declared > kLinkMaxPayload) {
      ++errors_;
      reset();
      return Status::kError;
    }
    expected_ = kLinkHeaderSize + declared;
  }
  if (length_ < expected_) {
    return Status::kIdle;
  }
  const Status status =
      (decodeLinkFrame(buffer_, length_, out) == LinkDecodeResult::kOk) ? Status::kFrameReady
                                                                       : Status::kError;
  if (status == Status::kError) {
    ++errors_;
  }
  reset();
  return status;
}

// ---------------------------------------------------------------------------
// Hop limited routing
// ---------------------------------------------------------------------------
void LinkRoutes::begin(uint8_t selfNode) {
  selfNode_ = selfNode;
  upstream_ = 0U;
  directMask_ = 0U;
  clearLearned();
}

void LinkRoutes::setUpstream(uint8_t node) {
  upstream_ = (node == 0U || node == selfNode_) ? 0U : node;
}

void LinkRoutes::setDirect(uint8_t node, bool direct) {
  if (node == 0U || node == selfNode_) {
    return;
  }
  const uint8_t bit = nodeBit(node);
  if (bit == 0U) {
    return;
  }
  if (direct) {
    directMask_ = static_cast<uint8_t>(directMask_ | bit);
  } else {
    directMask_ = static_cast<uint8_t>(directMask_ & ~bit);
  }
}

void LinkRoutes::clearDirect() {
  directMask_ = 0U;
}

bool LinkRoutes::isDirect(uint8_t node) const {
  return (directMask_ & nodeBit(node)) != 0U;
}

void LinkRoutes::clearLearned() {
  for (size_t i = 0; i < sizeof(via_); ++i) {
    via_[i] = 0U;
  }
}

void LinkRoutes::learnVia(uint8_t node, uint8_t via) {
  if (node == 0U || node > 8U || via == 0U || via > 8U || node == selfNode_ || node == via) {
    return;
  }
  if (isDirect(node)) {
    return;  // a direct socket always wins
  }
  via_[node] = via;
}

void LinkRoutes::forgetVia(uint8_t via) {
  if (via == 0U) {
    return;
  }
  for (size_t i = 0; i < sizeof(via_); ++i) {
    if (via_[i] == via) {
      via_[i] = 0U;
    }
  }
}

uint8_t LinkRoutes::viaOf(uint8_t node) const {
  return (node > 8U) ? 0U : via_[node];
}

uint8_t LinkRoutes::reachableMask() const {
  uint8_t mask = nodeBit(selfNode_);
  mask = static_cast<uint8_t>(mask | directMask_);
  for (uint8_t node = 1U; node <= 8U; ++node) {
    if (via_[node] != 0U) {
      mask = static_cast<uint8_t>(mask | nodeBit(node));
    }
  }
  return mask;
}

void LinkRoutes::applyReachableMask(uint8_t mask, uint8_t via) {
  if (via == 0U) {
    return;
  }
  for (uint8_t node = 1U; node <= 8U; ++node) {
    if ((mask & nodeBit(node)) == 0U) {
      continue;
    }
    if (node == selfNode_) {
      continue;
    }
    if (isDirect(node)) {
      continue;
    }
    if (node == via) {
      continue;  // the sender cannot be its own next hop
    }
    via_[node] = via;
  }
}

uint8_t LinkRoutes::nextHop(uint8_t dstNode) const {
  if (dstNode == 0U || dstNode == selfNode_) {
    return 0U;  // local, no link hop needed
  }
  if (isDirect(dstNode)) {
    return dstNode;
  }
  const uint8_t learned = viaOf(dstNode);
  if (learned != 0U) {
    return learned;
  }
  if (upstream_ != 0U && upstream_ != dstNode) {
    return upstream_;
  }
  return 0U;
}

bool LinkRoutes::reachable(uint8_t dstNode) const {
  return dstNode != 0U && (dstNode == selfNode_ || nextHop(dstNode) != 0U);
}

uint8_t LinkRoutes::relayTarget(uint8_t dstNode, uint8_t incomingNode, uint8_t hops) const {
  if (dstNode == 0U || dstNode == selfNode_) {
    return 0U;  // addressed to us: not a relay
  }
  if (hops >= kLinkMaxHops) {
    return 0U;  // path too long: drop instead of looping
  }
  const uint8_t next = nextHop(dstNode);
  if (next == 0U || next == incomingNode) {
    // No route, or the only route is the station the frame came from: dropping
    // is what makes a loop impossible even with a misconfigured topology.
    return 0U;
  }
  return next;
}

// ---------------------------------------------------------------------------
// Link health
// ---------------------------------------------------------------------------
void LinkClientState::begin(uint32_t nowMs, uint32_t retryMs) {
  connected_ = false;
  retryMs_ = (retryMs == 0U) ? 1000U : retryMs;
  nextAttemptMs_ = nowMs;
  attempts_ = 0U;
  reconnects_ = 0U;
}

void LinkClientState::onConnected(uint32_t nowMs) {
  if (!connected_) {
    ++reconnects_;
  }
  connected_ = true;
  nextAttemptMs_ = nowMs;
}

void LinkClientState::onDisconnected(uint32_t nowMs) {
  connected_ = false;
  nextAttemptMs_ = nowMs + retryMs_;
}

bool LinkClientState::shouldAttempt(uint32_t nowMs) const {
  return !connected_ && static_cast<int32_t>(nowMs - nextAttemptMs_) >= 0;
}

void LinkClientState::noteAttempt(uint32_t nowMs) {
  ++attempts_;
  nextAttemptMs_ = nowMs + retryMs_;
}

void LinkLiveness::begin(uint32_t nowMs) {
  lastActivityMs_ = nowMs;
}

void LinkLiveness::noteActivity(uint32_t nowMs) {
  lastActivityMs_ = nowMs;
}

bool LinkLiveness::stale(uint32_t nowMs, uint32_t timeoutMs) const {
  return static_cast<uint32_t>(nowMs - lastActivityMs_) > timeoutMs;
}

bool linkTokenMatches(const char* expected, size_t expectedLength, const char* provided) {
  // An empty expected token never authenticates anything: a misconfigured
  // station must not open its link to everybody.
  if (expected == nullptr || provided == nullptr || expectedLength == 0U) {
    return false;
  }
  const size_t providedLength = strlen(provided);
  uint8_t difference = static_cast<uint8_t>(providedLength == expectedLength ? 0U : 1U);
  for (size_t i = 0; i < expectedLength; ++i) {
    const uint8_t wanted = static_cast<uint8_t>(expected[i]);
    const uint8_t given =
        (i < providedLength) ? static_cast<uint8_t>(provided[i]) : static_cast<uint8_t>(0U);
    difference = static_cast<uint8_t>(difference | (wanted ^ given));
  }
  return difference == 0U;
}

const char* linkMessageTypeName(uint8_t type) {
  switch (type) {
    case kLinkHello: return "hello";
    case kLinkHelloAck: return "hello-ack";
    case kLinkReject: return "reject";
    case kLinkHeartbeat: return "heartbeat";
    case kLinkRouteState: return "route-state";
    case kLinkCallSignal: return "call-signal";
    case kLinkCallAudio: return "call-audio";
    default: return "unknown";
  }
}

const char* callSignalActionName(uint8_t action) {
  switch (action) {
    case kSignalInvite: return "invite";
    case kSignalRinging: return "ringing";
    case kSignalAccept: return "accept";
    case kSignalReject: return "reject";
    case kSignalCancel: return "cancel";
    case kSignalHangup: return "hangup";
    case kSignalEnded: return "ended";
    default: return "unknown";
  }
}

}  // namespace cell
