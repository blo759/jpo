#ifndef CELL_CALL_H
#define CELL_CALL_H

// Local only, real time audio call bookkeeping.
//
// This header is intentionally platform neutral (no Arduino dependency) and,
// more importantly, it deliberately does NOT include cell_protocol.h nor
// cell_core.h: a live call never travels on the shared RS485 bus. The module
// only knows about:
//   * a bounded table of two party call sessions,
//   * a membership probe supplied by the station (is this user connected to
//     *this* station right now?),
//   * the binary media frame format relayed by the station between the two
//     WebSocket clients of the same cell.
//
// Cellular isolation is enforced here, not in the UI: a user of another cell
// has no socket on this station, so the probe answers false and the call is
// refused. Nothing in this file can reach the mesh: there is no frame type, no
// queue and no bus access.
//
// Difference with the existing recorded voice notes: a voice note is a finite
// file fragmented over the bus and stored on the phone; a call is a continuous
// bidirectional PCM stream relayed between two local sockets.

#include <cstddef>
#include <cstdint>

namespace cell {

constexpr size_t kMaxCallSessions = 4;
constexpr size_t kCallIdLength = 8;     // ASCII alphanumeric, no terminator in the field
constexpr size_t kCallUserLength = 24;  // same bound as the routing table user ids

// An unanswered invitation is forgotten after this delay.
constexpr uint32_t kCallRingTimeoutMs = 30000;
// An established call with no media at all for this long is considered dead
// (the peer process died without closing its socket). A muted participant keeps
// sending silence frames, so this only fires on a real loss.
constexpr uint32_t kCallMediaTimeoutMs = 20000;
// Anti abuse: minimum delay between two invitations from the same caller.
constexpr uint32_t kCallInviteCooldownMs = 1500;

// ---------------------------------------------------------------------------
// Media frame (binary WebSocket payload; never a bus frame)
// ---------------------------------------------------------------------------
//   offset  size  field
//   0       1     magic 0xA7 (live call audio, distinct from 0xA5 voice notes)
//   1       1     version (1)
//   2       2     flags, reserved, 0
//   4       8     call id, ASCII, space padded
//   12      2     sequence, little endian, per direction
//   14      2     payload length, little endian
//   16      n     PCM payload (16 bit signed, mono, 8 kHz)
//
// 8 kHz mono 16 bit with 40 ms frames is 640 bytes of payload: 25 frames per
// second and per direction, which an ESP32 access point relays comfortably
// while the RS485 transport, the HTTP server and the WebSocket events keep
// running on the same loop.
constexpr uint8_t kCallAudioMagic = 0xA7;
constexpr uint8_t kCallAudioVersion = 1;
constexpr size_t kCallAudioHeaderSize = 16;
constexpr size_t kCallAudioFrameBytes = 640;
constexpr size_t kCallAudioMaxPayload = 1024;

enum class CallState : uint8_t { kIdle = 0, kRinging, kActive, kEnded };

enum class CallEndReason : uint8_t {
  kNone = 0,
  kRejected,
  kCancelled,
  kHungUp,
  kTimeout,
  kDisconnected,
  kBusy,
  kUnavailable,
  kError,
};

// Result of every state transition. Each value maps to an explicit HTTP status
// or WebSocket event, so a refusal is never silent.
enum class CallStatus : uint8_t {
  kOk = 0,
  kSelfCall,
  kNotLocal,        // caller or callee is not connected to this station
  kUnknownUser,
  kUnavailable,     // callee has no live socket on this station
  kBusy,            // one of the two users is already in a session
  kUnknownCall,
  kNotParticipant,
  kInvalidState,
  kExpired,
  kTableFull,
  kTooSoon,
  kInvalidCallId,
};

struct CallSession {
  bool used = false;
  char callId[kCallIdLength + 1] = {0};
  char caller[kCallUserLength] = {0};
  char callee[kCallUserLength] = {0};
  CallState state = CallState::kIdle;
  uint32_t ringingSinceMs = 0;
  uint32_t answeredAtMs = 0;
  uint32_t lastMediaMs = 0;
};

// Bounded, statically allocated call session table.
class CallTable {
 public:
  // Returns true when `userId` is an active local user of this station. The
  // station implements it from its WebSocket binding table, which is the only
  // trustworthy proof of "this user is on my cell": an identifier declared by a
  // client is never enough, and a user of another cell has no local socket.
  using LocalUserProbe = bool (*)(void* context, const char* userId);

  void begin(LocalUserProbe probe, void* context);

  // Signalling transitions. All of them validate the caller/callee pair against
  // the local probe before touching the table.
  CallStatus invite(const char* callId, const char* caller, const char* callee, uint32_t nowMs);
  CallStatus accept(const char* callId, const char* userId, uint32_t nowMs);
  CallStatus reject(const char* callId, const char* userId, uint32_t nowMs);
  CallStatus cancel(const char* callId, const char* userId, uint32_t nowMs);
  CallStatus hangup(const char* callId, const char* userId, uint32_t nowMs);

  // Ends the session a user takes part in (socket closed, presence lost).
  // `peerOut` and `callIdOut` receive what the station needs to notify the other
  // party, so resources are never leaked on a disconnection.
  CallStatus endForUser(const char* userId,
                        CallEndReason reason,
                        uint32_t nowMs,
                        char* peerOut,
                        size_t peerCapacity,
                        char* callIdOut,
                        size_t callIdCapacity);

  // Expires at most one ringing session (call it from loop()). Returns true and
  // fills callIdOut when a session was dropped.
  bool expireRinging(uint32_t nowMs, char* callIdOut, size_t callIdCapacity);

  // Expires at most one established session that stopped carrying media. Both
  // participants are reported so the station can notify them without keeping a
  // second copy of the session.
  bool expireStaleMedia(uint32_t nowMs,
                        char* callIdOut,
                        size_t callIdCapacity,
                        char* callerOut,
                        size_t callerCapacity,
                        char* calleeOut,
                        size_t calleeCapacity);

  // Records that a media frame really crossed this station.
  void noteMedia(const char* callId, uint32_t nowMs);

  const CallSession* find(const char* callId) const;
  const CallSession* sessionOf(const char* userId) const;
  bool isBusy(const char* userId) const;

  // Validates an inbound media frame: returns the peer user id the frame must be
  // relayed to, or nullptr when the sender is not a participant of an *active*
  // session. This is what makes the relay safe: audio is only ever forwarded
  // inside an established two party session of this station.
  const char* mediaPeer(const char* callId, const char* sender) const;

  size_t activeSessions() const;
  uint32_t completedCalls() const;
  uint32_t refusedCalls() const;
  const char* lastRefusal() const { return lastRefusal_; }

  void clear();

 private:
  struct InviteStamp {
    char user[kCallUserLength] = {0};
    uint32_t atMs = 0;
  };

  CallSession* findMutable(const char* callId);
  CallSession* allocate();
  void release(CallSession& session);
  void refuse(CallStatus status);

  CallSession sessions_[kMaxCallSessions];
  InviteStamp stamps_[kMaxCallSessions];
  LocalUserProbe probe_ = nullptr;
  void* probeContext_ = nullptr;
  uint32_t completed_ = 0;
  uint32_t refused_ = 0;
  const char* lastRefusal_ = "";
};

// ---------------------------------------------------------------------------
// Media frame helpers (pure functions, unit tested)
// ---------------------------------------------------------------------------
// Encodes one media frame. Returns the number of bytes to write, or 0 when the
// call id, the payload or the output buffer is invalid.
size_t buildCallAudioFrame(const char* callId,
                           uint16_t sequence,
                           const uint8_t* payload,
                           size_t payloadLength,
                           uint8_t* out,
                           size_t capacity);

// Decodes one media frame. Returns false on a wrong magic, a wrong version, an
// invalid call id, an inconsistent length or a truncated buffer.
bool parseCallAudioFrame(const uint8_t* data,
                         size_t length,
                         char* callIdOut,
                         size_t callIdCapacity,
                         uint16_t& sequenceOut,
                         const uint8_t** payloadOut,
                         size_t& payloadLengthOut);

bool isValidCallId(const char* callId);

const char* callStatusName(CallStatus status);
const char* callEndReasonName(CallEndReason reason);
const char* callStateName(CallState state);

}  // namespace cell

#endif  // CELL_CALL_H
