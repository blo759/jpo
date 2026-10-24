#ifndef CELL_LINK_H
#define CELL_LINK_H

// Inter-station IP link of CellNet.
//
// The three stations each run their own access point on separate subnets, so
// there is no IP path between them by default. This module describes the
// *backbone* that creates one: a length prefixed binary protocol carried by a
// TCP stream between stations, used only for two things:
//   * call signalling between cells,
//   * call audio relayed from one cell to another.
//
// Like lib/cell_call, this header deliberately does NOT include
// cell_protocol.h nor cell_core.h: call audio and cross cell signalling never
// travel on the shared RS485 bus. The mesh keeps its token arbitration, its
// priority queues and its acknowledgements untouched.
//
// Topology: a star with one hub. A leaf station opens the TCP connection toward
// the hub's known access point address; the hub answers on the already open
// socket, so no peer address discovery, no NAT and no IP route is required.
// The hop counter and the "never send back to the sender" rule make a relayed
// frame impossible to loop.

#include <cstddef>
#include <cstdint>

namespace cell {

constexpr uint8_t kLinkMagic = 0xC3;
constexpr uint8_t kLinkVersion = 1;
constexpr size_t kLinkHeaderSize = 12;
// One call media frame is 16 + 640 = 656 bytes; the bound leaves margin for the
// signalling payloads.
constexpr size_t kLinkMaxPayload = 704;
constexpr size_t kLinkMaxFrameSize = kLinkHeaderSize + kLinkMaxPayload;
// Outbound buffer of one link socket. One audio frame is 668 bytes, so 2 kB
// absorbs a short congestion without ever dropping a call signal.
constexpr size_t kLinkTxBufferSize = 2048;
// A call signal is tiny and must never be blocked by a burst of call audio:
// the two kinds of frame have their own ring, exactly like the bus control and
// application rings.
constexpr size_t kLinkTxControlSize = 256;
constexpr size_t kLinkTokenLength = 24;
constexpr size_t kLinkUserLength = 24;
constexpr size_t kLinkCallIdLength = 8;
// A relayed frame may cross at most two stations (leaf -> hub -> leaf).
constexpr uint8_t kLinkMaxHops = 3;
// Direct peers a station can hold at once (the hub holds two leaves).
constexpr size_t kLinkMaxPeers = 2;

// Wire format of a link frame (before the length is written on the stream):
//
//   offset  size  field
//   0       1     magic 0xC3
//   1       1     version (1)
//   2       1     type            (see LinkMessageType)
//   3       1     flags           (reserved, 0)
//   4       1     srcNode         1..3
//   5       1     dstNode         1..3, the final destination (not the next hop)
//   6       1     hops            incremented by every relay
//   7       1     reserved        0
//   8       4     payloadLength   little endian
//   12      n     payload
//
// A reader first consumes the 12 byte header, validates the magic and the
// version, then reads exactly `payloadLength` more bytes: the total frame size is
// always kLinkHeaderSize + payloadLength, so a corrupted stream resynchronises
// on the next valid header instead of desynchronising for ever.

enum LinkMessageType : uint8_t {
  // The link protocol has its own numbering range, deliberately disjoint from
  // the RS485 frame types: a link frame can never be confused with a bus frame,
  // and the two protocols cannot drift into a collision.
  kLinkHello = 0x81,       // leaf -> hub: identity + shared token
  kLinkHelloAck = 0x82,    // hub -> leaf: accepted
  kLinkReject = 0x83,      // hub -> leaf: token refused, the socket is closed
  kLinkHeartbeat = 0x84,   // liveness, both directions
  kLinkRouteState = 0x85,  // nodes the sender can reach (1 byte mask)
  kLinkCallSignal = 0x90,  // cross cell call signalling
  kLinkCallAudio = 0x91,   // call audio relayed between two cells
};

// Payload of kLinkRouteState: one byte, bit i set when the sender can reach the
// station whose node id is i+1 (itself included). It lets a chain converge
// without waiting for real traffic, and it is what makes a station aware of a
// cell it has no direct socket to.
constexpr size_t kLinkRouteStatePayloadSize = 1;

// Signalling actions exchanged between the two stations of a call.
enum CallSignalAction : uint8_t {
  kSignalInvite = 1,   // caller station  -> callee station
  kSignalRinging = 2,  // callee station  -> caller station (callee is ringing)
  kSignalAccept = 3,   // callee station  -> caller station
  kSignalReject = 4,   // callee station  -> caller station
  kSignalCancel = 5,   // caller station  -> callee station
  kSignalHangup = 6,   // either direction, established call ended
  kSignalEnded = 7,    // either direction, with a reason (peer left, error)
};

struct LinkFrame {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint8_t srcNode = 0;
  uint8_t dstNode = 0;
  uint8_t hops = 0;
  uint16_t payloadLength = 0;
  uint8_t payload[kLinkMaxPayload] = {0};
};

// Serialises one frame. Returns the number of bytes to write, or 0 on error.
size_t encodeLinkFrame(const LinkFrame& frame, uint8_t* out, size_t capacity);

enum class LinkDecodeResult : uint8_t { kOk, kInvalid };

// Decodes a complete frame (length prefix already removed). The declared length
// must match the buffer exactly.
LinkDecodeResult decodeLinkFrame(const uint8_t* data, size_t length, LinkFrame& out);

// Returns the declared payload length of a header, or 0 when the header is not
// a valid link header.
uint16_t linkDeclaredLength(const uint8_t* header, size_t length);

// ---------------------------------------------------------------------------
// Call signalling payload
// ---------------------------------------------------------------------------
// Fixed 58 bytes, zero padded: no variable length parsing, so a truncated or
// corrupted signalling message can never be misread as another action.
//
//   offset  size  field
//   0       8     callId, ASCII alphanumeric, zero padded
//   8       1     action           (see CallSignalAction)
//   9       1     reason           (callEndReasonName value, 0 when not used)
//   10      24    from user id     (the caller, whatever the direction)
//   34      24    to user id       (the callee, whatever the direction)
constexpr size_t kLinkCallSignalSize = kLinkCallIdLength + 2 + kLinkUserLength + kLinkUserLength;

struct LinkCallSignal {
  char callId[kLinkCallIdLength + 1] = {0};
  uint8_t action = 0;
  uint8_t reason = 0;
  char from[kLinkUserLength] = {0};
  char to[kLinkUserLength] = {0};
};

size_t buildLinkCallSignal(const LinkCallSignal& signal, uint8_t* out, size_t capacity);
bool parseLinkCallSignal(const uint8_t* data, size_t length, LinkCallSignal& out);

// ---------------------------------------------------------------------------
// Authentication handshake payload
// ---------------------------------------------------------------------------
// The station that opens the connection announces its identity and proves it
// knows the shared token. The token is compared in constant time by the server
// and is never echoed back, so a rejected station learns nothing.
//
//   offset  size  field
//   0       1     nodeId
//   1       24    token, zero padded, always terminated
constexpr size_t kLinkHelloPayloadSize = 1 + kLinkTokenLength;

size_t buildLinkHello(uint8_t nodeId, const char* token, uint8_t* out, size_t capacity);
bool parseLinkHello(const uint8_t* data,
                    size_t length,
                    uint8_t& nodeIdOut,
                    char* tokenOut,
                    size_t tokenCapacity);

// ---------------------------------------------------------------------------
// Stream reassembly
// ---------------------------------------------------------------------------
// A TCP stream delivers arbitrary chunks: this assembler rebuilds frames from a
// byte flow and resynchronises on the next length prefix after an error.
class LinkStreamAssembler {
 public:
  enum class Status : uint8_t { kIdle, kFrameReady, kError };

  Status push(uint8_t byte, LinkFrame& out);
  void reset();
  uint32_t errors() const { return errors_; }
  size_t buffered() const { return length_; }

 private:
  uint8_t buffer_[kLinkMaxFrameSize] = {0};
  size_t length_ = 0;
  size_t expected_ = 0;
  uint32_t errors_ = 0;
};

// ---------------------------------------------------------------------------
// Hop limited routing
// ---------------------------------------------------------------------------
// The topology is a star (one hub, the other stations are leaves), but the table
// itself is generic: a node is reachable either directly (an established socket)
// or through the upstream station it is attached to. The relay never sends a
// frame back to the station it came from, and the hop counter caps the path, so
// a frame cannot loop.
class LinkRoutes {
 public:
  void begin(uint8_t selfNode);
  void setUpstream(uint8_t node);
  void setDirect(uint8_t node, bool direct);
  void clearDirect();

  uint8_t selfNode() const { return selfNode_; }
  uint8_t upstream() const { return upstream_; }
  bool isDirect(uint8_t node) const;
  uint8_t directMask() const { return directMask_; }

  // Learned routes: a node reached through another station. Used by a chain
  // topology, where a station has no direct socket to a distant cell.
  void learnVia(uint8_t node, uint8_t via);
  void forgetVia(uint8_t via);
  uint8_t viaOf(uint8_t node) const;
  void clearLearned();

  /** Nodes this station can deliver to, itself included. */
  uint8_t reachableMask() const;
  /** Applies the reachability mask announced by `via`. */
  void applyReachableMask(uint8_t mask, uint8_t via);

  /** Next station to hand a frame addressed to `dstNode` to, or 0. */
  uint8_t nextHop(uint8_t dstNode) const;
  bool reachable(uint8_t dstNode) const;

  /** Relay decision: the station to forward to, or 0 when it must be dropped. */
  uint8_t relayTarget(uint8_t dstNode, uint8_t incomingNode, uint8_t hops) const;

 private:
  uint8_t selfNode_ = 0;
  uint8_t upstream_ = 0;
  uint8_t directMask_ = 0;
  // Indexed by node id (0..8); 0 means "no learned route".
  uint8_t via_[9] = {0};
};

// ---------------------------------------------------------------------------
// Non blocking outbound buffer
// ---------------------------------------------------------------------------
// A TCP socket must never be written with the library write(): on ESP32 it
// retries up to 10 times with a 1 second select() each, so a congested link can
// stall the caller for seconds. That is fatal here, because the caller is the
// main loop, which also has to drain the RS485 UART before its buffer overflows.
//
// This buffer therefore holds whole frames and is drained by the caller with at
// most one non blocking write per loop. The byte order is preserved and a
// partially written frame is never discarded, so the peer stream stays valid
// even when the socket only accepts part of a frame.
class LinkTxBuffer {
 public:
  // `storage` must outlive the buffer.
  LinkTxBuffer(uint8_t* storage, size_t capacity);

  // Returns the number of bytes accepted, or 0 when the buffer is full.
  size_t push(const uint8_t* bytes, size_t count);

  // Platform neutral: the caller performs the actual write.
  // Must return the number of bytes written, 0 when nothing could be written.
  using WriteFn = size_t (*)(void* context, const uint8_t* data, size_t length);

  // Writes at most one slice. Returns the number of bytes written.
  size_t flush(WriteFn write, void* context);

  void clear();
  bool empty() const { return length_ == 0U; }
  size_t size() const { return length_ - sent_; }
  size_t capacity() const { return capacity_; }
  uint32_t rejected() const { return rejected_; }

 private:
  void compact();

  uint8_t* data_ = nullptr;
  size_t capacity_ = 0;
  size_t length_ = 0;
  size_t sent_ = 0;
  uint32_t rejected_ = 0;
};

// ---------------------------------------------------------------------------
// Link health
// ---------------------------------------------------------------------------
// Client side reconnection policy of a leaf station (the station that opens the
// socket toward the hub). Pure arithmetic so it is unit tested without a radio.
class LinkClientState {
 public:
  void begin(uint32_t nowMs, uint32_t retryMs);
  void onConnected(uint32_t nowMs);
  void onDisconnected(uint32_t nowMs);
  bool shouldAttempt(uint32_t nowMs) const;
  /** Records that a connection attempt was started at `nowMs`. */
  void noteAttempt(uint32_t nowMs);
  /** Replaces the delay used by the next onDisconnected() (progressive backoff). */
  void setRetryDelay(uint32_t retryMs);
  bool connected() const { return connected_; }
  uint32_t attempts() const { return attempts_; }
  uint32_t reconnects() const { return reconnects_; }
  uint32_t retryDelayMs() const { return retryMs_; }

 private:
  bool connected_ = false;
  uint32_t nextAttemptMs_ = 0;
  uint32_t retryMs_ = 2000;
  uint32_t attempts_ = 0;
  uint32_t reconnects_ = 0;
};

/** Last activity watchdog of one link socket. */
class LinkLiveness {
 public:
  void begin(uint32_t nowMs);
  void noteActivity(uint32_t nowMs);
  bool stale(uint32_t nowMs, uint32_t timeoutMs) const;
  uint32_t lastActivityMs() const { return lastActivityMs_; }

 private:
  uint32_t lastActivityMs_ = 0;
};

// Constant time comparison of the shared link token (never a plain strcmp: the
// comparison time must not leak the length of the expected secret).
bool linkTokenMatches(const char* expected, size_t expectedLength, const char* provided);

const char* linkMessageTypeName(uint8_t type);
const char* callSignalActionName(uint8_t action);

}  // namespace cell

#endif  // CELL_LINK_H
