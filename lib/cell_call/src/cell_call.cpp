#include "cell_call.h"

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

bool sameText(const char* left, const char* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  return strcmp(left, right) == 0;
}

// Reads the little endian 16 bit field at `offset` without any alignment or
// endianness assumption (the frame is validated byte by byte).
uint16_t readLe16(const uint8_t* data, size_t offset) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[offset]) |
                               (static_cast<uint16_t>(data[offset + 1U]) << 8));
}

void writeLe16(uint8_t* data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xFFU);
  data[offset + 1U] = static_cast<uint8_t>(value >> 8);
}

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool isValidCallId(const char* callId) {
  if (callId == nullptr) {
    return false;
  }
  for (size_t i = 0; i < kCallIdLength; ++i) {
    const char value = callId[i];
    const bool alphanumeric = (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
                              (value >= 'A' && value <= 'Z');
    if (!alphanumeric) {
      return false;
    }
  }
  // Exactly kCallIdLength characters, no more: a longer identifier would be
  // truncated and two calls could collide.
  return callId[kCallIdLength] == '\0';
}

const char* callStatusName(CallStatus status) {
  switch (status) {
    case CallStatus::kOk: return "ok";
    case CallStatus::kSelfCall: return "self-call";
    case CallStatus::kNotLocal: return "cross-cell";
    case CallStatus::kUnknownUser: return "unknown-user";
    case CallStatus::kUnavailable: return "unavailable";
    case CallStatus::kBusy: return "busy";
    case CallStatus::kUnknownCall: return "unknown-call";
    case CallStatus::kNotParticipant: return "not-participant";
    case CallStatus::kInvalidState: return "invalid-state";
    case CallStatus::kExpired: return "expired";
    case CallStatus::kTableFull: return "call-table-full";
    case CallStatus::kTooSoon: return "call-too-soon";
    case CallStatus::kInvalidCallId: return "invalid-call-id";
  }
  return "error";
}

const char* callEndReasonName(CallEndReason reason) {
  switch (reason) {
    case CallEndReason::kNone: return "none";
    case CallEndReason::kRejected: return "rejected";
    case CallEndReason::kCancelled: return "cancelled";
    case CallEndReason::kHungUp: return "hangup";
    case CallEndReason::kTimeout: return "timeout";
    case CallEndReason::kDisconnected: return "disconnected";
    case CallEndReason::kBusy: return "busy";
    case CallEndReason::kUnavailable: return "unavailable";
    case CallEndReason::kError: return "error";
  }
  return "none";
}

const char* callStateName(CallState state) {
  switch (state) {
    case CallState::kIdle: return "idle";
    case CallState::kRinging: return "ringing";
    case CallState::kActive: return "active";
    case CallState::kEnded: return "ended";
  }
  return "idle";
}

// ---------------------------------------------------------------------------
// CallTable
// ---------------------------------------------------------------------------
void CallTable::begin(LocalUserProbe probe, void* context) {
  probe_ = probe;
  probeContext_ = context;
  clear();
}

void CallTable::clear() {
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    sessions_[i] = CallSession();
    stamps_[i] = InviteStamp();
  }
  completed_ = 0;
  refused_ = 0;
  lastRefusal_ = "";
}

CallSession* CallTable::findMutable(const char* callId) {
  if (callId == nullptr) {
    return nullptr;
  }
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    if (sessions_[i].used && sameText(sessions_[i].callId, callId)) {
      return &sessions_[i];
    }
  }
  return nullptr;
}

const CallSession* CallTable::find(const char* callId) const {
  return const_cast<CallTable*>(this)->findMutable(callId);
}

const CallSession* CallTable::sessionOf(const char* userId) const {
  if (userId == nullptr || userId[0] == '\0') {
    return nullptr;
  }
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    const CallSession& session = sessions_[i];
    if (!session.used) {
      continue;
    }
    if (sameText(session.caller, userId) || sameText(session.callee, userId)) {
      return &session;
    }
  }
  return nullptr;
}

bool CallTable::isBusy(const char* userId) const {
  return sessionOf(userId) != nullptr;
}

CallSession* CallTable::allocate() {
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    if (!sessions_[i].used) {
      return &sessions_[i];
    }
  }
  return nullptr;
}

void CallTable::release(CallSession& session) {
  session.used = false;
  session.callId[0] = '\0';
  session.caller[0] = '\0';
  session.callee[0] = '\0';
  session.state = CallState::kIdle;
  session.ringingSinceMs = 0;
  session.answeredAtMs = 0;
  session.lastMediaMs = 0;
}

void CallTable::refuse(CallStatus status) {
  ++refused_;
  lastRefusal_ = callStatusName(status);
}

size_t CallTable::activeSessions() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    if (sessions_[i].used) {
      ++count;
    }
  }
  return count;
}

uint32_t CallTable::completedCalls() const {
  return completed_;
}

uint32_t CallTable::refusedCalls() const {
  return refused_;
}

// ---------------------------------------------------------------------------
// Signalling
// ---------------------------------------------------------------------------
CallStatus CallTable::invite(const char* callId,
                            const char* caller,
                            const char* callee,
                            uint32_t nowMs) {
  if (!isValidCallId(callId)) {
    refuse(CallStatus::kInvalidCallId);
    return CallStatus::kInvalidCallId;
  }
  if (caller == nullptr || caller[0] == '\0' || callee == nullptr || callee[0] == '\0') {
    refuse(CallStatus::kUnknownUser);
    return CallStatus::kUnknownUser;
  }
  if (sameText(caller, callee)) {
    refuse(CallStatus::kSelfCall);
    return CallStatus::kSelfCall;
  }
  // The membership probe is the whole cellular isolation: a user of another
  // station is not a local user here and can therefore never be called.
  if (probe_ == nullptr || !probe_(probeContext_, caller)) {
    refuse(CallStatus::kNotLocal);
    return CallStatus::kNotLocal;
  }
  if (!probe_(probeContext_, callee)) {
    refuse(CallStatus::kNotLocal);
    return CallStatus::kNotLocal;
  }
  if (findMutable(callId) != nullptr) {
    refuse(CallStatus::kBusy);
    return CallStatus::kBusy;
  }
  if (isBusy(caller) || isBusy(callee)) {
    refuse(CallStatus::kBusy);
    return CallStatus::kBusy;
  }

  // Anti abuse: a caller cannot flood invitations.
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    if (stamps_[i].user[0] != '\0' && sameText(stamps_[i].user, caller) &&
        static_cast<uint32_t>(nowMs - stamps_[i].atMs) < kCallInviteCooldownMs) {
      refuse(CallStatus::kTooSoon);
      return CallStatus::kTooSoon;
    }
  }

  CallSession* session = allocate();
  if (session == nullptr) {
    refuse(CallStatus::kTableFull);
    return CallStatus::kTableFull;
  }
  release(*session);
  session->used = true;
  copyBounded(session->callId, sizeof(session->callId), callId);
  copyBounded(session->caller, sizeof(session->caller), caller);
  copyBounded(session->callee, sizeof(session->callee), callee);
  session->state = CallState::kRinging;
  session->ringingSinceMs = nowMs;
  session->lastMediaMs = nowMs;

  // Remember the invitation time for the cooldown, in a slot that survives the
  // release of the session itself.
  size_t slot = 0;
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    if (sameText(stamps_[i].user, caller)) {
      slot = i;
      break;
    }
    if (stamps_[i].user[0] == '\0') {
      slot = i;
      break;
    }
    slot = i;
  }
  copyBounded(stamps_[slot].user, sizeof(stamps_[slot].user), caller);
  stamps_[slot].atMs = nowMs;

  return CallStatus::kOk;
}

namespace {

// A user must be one of the two participants; the caller can never accept or
// reject its own invitation.
bool isCallee(const CallSession& session, const char* userId) {
  return sameText(session.callee, userId);
}

bool isCaller(const CallSession& session, const char* userId) {
  return sameText(session.caller, userId);
}

bool isParticipant(const CallSession& session, const char* userId) {
  return isCaller(session, userId) || isCallee(session, userId);
}

const char* otherParty(const CallSession& session, const char* userId) {
  if (isCaller(session, userId)) {
    return session.callee;
  }
  if (isCallee(session, userId)) {
    return session.caller;
  }
  return nullptr;
}

}  // namespace

CallStatus CallTable::accept(const char* callId, const char* userId, uint32_t nowMs) {
  CallSession* session = findMutable(callId);
  if (session == nullptr) {
    refuse(CallStatus::kUnknownCall);
    return CallStatus::kUnknownCall;
  }
  if (!isCallee(*session, userId)) {
    refuse(CallStatus::kNotParticipant);
    return CallStatus::kNotParticipant;
  }
  if (session->state != CallState::kRinging) {
    refuse(CallStatus::kInvalidState);
    return CallStatus::kInvalidState;
  }
  if (static_cast<uint32_t>(nowMs - session->ringingSinceMs) > kCallRingTimeoutMs) {
    // An expired invitation can never be accepted: the session is dropped so a
    // late acceptance cannot resurrect a call nobody is waiting for.
    release(*session);
    refuse(CallStatus::kExpired);
    return CallStatus::kExpired;
  }
  session->state = CallState::kActive;
  session->answeredAtMs = nowMs;
  session->lastMediaMs = nowMs;
  return CallStatus::kOk;
}

CallStatus CallTable::reject(const char* callId, const char* userId, uint32_t nowMs) {
  (void)nowMs;
  CallSession* session = findMutable(callId);
  if (session == nullptr) {
    refuse(CallStatus::kUnknownCall);
    return CallStatus::kUnknownCall;
  }
  if (!isCallee(*session, userId)) {
    refuse(CallStatus::kNotParticipant);
    return CallStatus::kNotParticipant;
  }
  if (session->state != CallState::kRinging) {
    refuse(CallStatus::kInvalidState);
    return CallStatus::kInvalidState;
  }
  release(*session);
  return CallStatus::kOk;
}

CallStatus CallTable::cancel(const char* callId, const char* userId, uint32_t nowMs) {
  (void)nowMs;
  CallSession* session = findMutable(callId);
  if (session == nullptr) {
    refuse(CallStatus::kUnknownCall);
    return CallStatus::kUnknownCall;
  }
  if (!isCaller(*session, userId)) {
    refuse(CallStatus::kNotParticipant);
    return CallStatus::kNotParticipant;
  }
  if (session->state != CallState::kRinging) {
    refuse(CallStatus::kInvalidState);
    return CallStatus::kInvalidState;
  }
  release(*session);
  return CallStatus::kOk;
}

CallStatus CallTable::hangup(const char* callId, const char* userId, uint32_t nowMs) {
  (void)nowMs;
  CallSession* session = findMutable(callId);
  if (session == nullptr) {
    refuse(CallStatus::kUnknownCall);
    return CallStatus::kUnknownCall;
  }
  if (!isParticipant(*session, userId)) {
    refuse(CallStatus::kNotParticipant);
    return CallStatus::kNotParticipant;
  }
  if (session->state == CallState::kActive) {
    ++completed_;
  }
  release(*session);
  return CallStatus::kOk;
}

CallStatus CallTable::endForUser(const char* userId,
                                 CallEndReason reason,
                                 uint32_t nowMs,
                                 char* peerOut,
                                 size_t peerCapacity,
                                 char* callIdOut,
                                 size_t callIdCapacity) {
  (void)nowMs;
  (void)reason;
  if (peerOut != nullptr && peerCapacity > 0U) {
    peerOut[0] = '\0';
  }
  if (callIdOut != nullptr && callIdCapacity > 0U) {
    callIdOut[0] = '\0';
  }
  CallSession* session = const_cast<CallSession*>(sessionOf(userId));
  if (session == nullptr) {
    return CallStatus::kUnknownCall;
  }
  const char* peer = otherParty(*session, userId);
  if (peerOut != nullptr && peerCapacity > 0U) {
    copyBounded(peerOut, peerCapacity, peer);
  }
  if (callIdOut != nullptr && callIdCapacity > 0U) {
    copyBounded(callIdOut, callIdCapacity, session->callId);
  }
  if (session->state == CallState::kActive) {
    ++completed_;
  }
  release(*session);
  return CallStatus::kOk;
}

bool CallTable::expireRinging(uint32_t nowMs, char* callIdOut, size_t callIdCapacity) {
  if (callIdOut != nullptr && callIdCapacity > 0U) {
    callIdOut[0] = '\0';
  }
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    CallSession& session = sessions_[i];
    if (!session.used || session.state != CallState::kRinging) {
      continue;
    }
    if (static_cast<uint32_t>(nowMs - session.ringingSinceMs) <= kCallRingTimeoutMs) {
      continue;
    }
    if (callIdOut != nullptr && callIdCapacity > 0U) {
      copyBounded(callIdOut, callIdCapacity, session.callId);
    }
    release(session);
    return true;
  }
  return false;
}

bool CallTable::expireStaleMedia(uint32_t nowMs,
                                 char* callIdOut,
                                 size_t callIdCapacity,
                                 char* callerOut,
                                 size_t callerCapacity,
                                 char* calleeOut,
                                 size_t calleeCapacity) {
  if (callIdOut != nullptr && callIdCapacity > 0U) {
    callIdOut[0] = '\0';
  }
  if (callerOut != nullptr && callerCapacity > 0U) {
    callerOut[0] = '\0';
  }
  if (calleeOut != nullptr && calleeCapacity > 0U) {
    calleeOut[0] = '\0';
  }
  for (size_t i = 0; i < kMaxCallSessions; ++i) {
    CallSession& session = sessions_[i];
    if (!session.used || session.state != CallState::kActive) {
      continue;
    }
    if (static_cast<uint32_t>(nowMs - session.lastMediaMs) <= kCallMediaTimeoutMs) {
      continue;
    }
    if (callIdOut != nullptr && callIdCapacity > 0U) {
      copyBounded(callIdOut, callIdCapacity, session.callId);
    }
    // Both participants are reported: the station notifies each of them.
    if (callerOut != nullptr && callerCapacity > 0U) {
      copyBounded(callerOut, callerCapacity, session.caller);
    }
    if (calleeOut != nullptr && calleeCapacity > 0U) {
      copyBounded(calleeOut, calleeCapacity, session.callee);
    }
    ++completed_;
    release(session);
    return true;
  }
  return false;
}

void CallTable::noteMedia(const char* callId, uint32_t nowMs) {
  CallSession* session = findMutable(callId);
  if (session == nullptr) {
    return;
  }
  session->lastMediaMs = nowMs;
}

const char* CallTable::mediaPeer(const char* callId, const char* sender) const {
  const CallSession* session = find(callId);
  if (session == nullptr || session->state != CallState::kActive) {
    return nullptr;
  }
  return otherParty(*session, sender);
}

// ---------------------------------------------------------------------------
// Media frame
// ---------------------------------------------------------------------------
size_t buildCallAudioFrame(const char* callId,
                           uint16_t sequence,
                           const uint8_t* payload,
                           size_t payloadLength,
                           uint8_t* out,
                           size_t capacity) {
  if (!isValidCallId(callId) || out == nullptr || payload == nullptr || payloadLength == 0U ||
      payloadLength > kCallAudioMaxPayload) {
    return 0U;
  }
  const size_t total = kCallAudioHeaderSize + payloadLength;
  if (capacity < total) {
    return 0U;
  }
  memset(out, 0, kCallAudioHeaderSize);
  out[0] = kCallAudioMagic;
  out[1] = kCallAudioVersion;
  // bytes 2..3: flags, reserved, left to zero
  memcpy(out + 4U, callId, kCallIdLength);
  writeLe16(out, 12U, sequence);
  writeLe16(out, 14U, static_cast<uint16_t>(payloadLength));
  memcpy(out + kCallAudioHeaderSize, payload, payloadLength);
  return total;
}

bool parseCallAudioFrame(const uint8_t* data,
                         size_t length,
                         char* callIdOut,
                         size_t callIdCapacity,
                         uint16_t& sequenceOut,
                         const uint8_t** payloadOut,
                         size_t& payloadLengthOut) {
  if (payloadOut != nullptr) {
    *payloadOut = nullptr;
  }
  payloadLengthOut = 0U;
  sequenceOut = 0U;
  if (callIdOut != nullptr && callIdCapacity > 0U) {
    callIdOut[0] = '\0';
  }
  if (payloadOut == nullptr || data == nullptr || length < kCallAudioHeaderSize ||
      data[0] != kCallAudioMagic || data[1] != kCallAudioVersion) {
    return false;
  }
  if (callIdOut == nullptr || callIdCapacity <= kCallIdLength) {
    return false;
  }
  char callId[kCallIdLength + 1] = {0};
  memcpy(callId, data + 4U, kCallIdLength);
  if (!isValidCallId(callId)) {
    return false;
  }
  const size_t declared = readLe16(data, 14U);
  if (declared == 0U || declared > kCallAudioMaxPayload) {
    return false;
  }
  // The declared length must match the real payload: a frame claiming more bytes
  // than it carries is rejected instead of being relayed truncated.
  if (length != kCallAudioHeaderSize + declared) {
    return false;
  }
  copyBounded(callIdOut, callIdCapacity, callId);
  sequenceOut = readLe16(data, 12U);
  *payloadOut = data + kCallAudioHeaderSize;
  payloadLengthOut = declared;
  return true;
}

}  // namespace cell
