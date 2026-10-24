#ifndef CELL_PROTOCOL_H
#define CELL_PROTOCOL_H

// Framing protocol shared by the three stations of the local cellular network.
//
// This header is intentionally platform neutral (no Arduino dependency) so the
// exact same code is compiled for the ESP32 firmware and for the native unit
// tests (env:native).
//
// Wire format (before COBS encoding):
//
//   offset  size  field
//   0       1     version
//   1       1     type           (see MessageType)
//   2       1     flags          (see kFlag*)
//   3       1     srcNode        1..3, never 0
//   4       1     dstNode        1..3, or kNodeBroadcast
//   5       2     sequence       little endian, per-sender increment
//   7       2     payloadLength  little endian, 0..kMaxBusPayload
//   9       n     payload
//   9+n     2     crc16          CCITT-FALSE over bytes [0, 9+n)
//
// The frame is then COBS encoded and terminated by a single 0x00 delimiter.
// COBS guarantees that the delimiter never appears inside a frame, so a
// receiver always recovers synchronisation on the next delimiter even after a
// truncated or corrupted frame.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace cell {

constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kNodeBroadcast = 0;
constexpr uint8_t kMaxStationNodes = 3;
constexpr size_t kMaxBusPayload = 480;
constexpr size_t kFrameHeaderSize = 9;
constexpr size_t kMaxRawFrameSize = kFrameHeaderSize + kMaxBusPayload + 2;
// Worst case COBS overhead: one code byte every 254 bytes, plus the first code
// byte, plus the 0x00 delimiter. Rounded up for safety.
constexpr size_t kMaxEncodedFrameSize = kMaxRawFrameSize + (kMaxRawFrameSize / 254U) + 4U;

enum MessageType : uint8_t {
  kTypePing = 0x01,          // node alive, broadcast
  kTypePong = 0x02,          // reply to a ping
  kTypeToken = 0x03,         // bus arbitration token, payload: next node
  kTypeRouteState = 0x04,    // full presence list of the emitting station
  kTypeRouteRequest = 0x05,  // ask every peer to publish its presence list
  kTypeBusAck = 0x06,        // transport level acknowledgement, payload: seq
  kTypeText = 0x10,          // opaque application text envelope
  kTypeReceipt = 0x11,       // opaque application receipt (delivered/read)
  kTypeVoiceBlock = 0x12,    // opaque application voice block
  kTypeVoiceFeedback = 0x13, // opaque application voice ack/nack
  kTypeAppControl = 0x14,    // opaque application control message
};

constexpr uint8_t kFlagAckRequired = 0x01;
constexpr uint8_t kFlagRetransmit = 0x02;
constexpr uint8_t kFlagFragmented = 0x04;
constexpr uint8_t kFlagTokenPass = 0x08;

struct Frame {
  uint8_t version = kProtocolVersion;
  uint8_t type = static_cast<uint8_t>(kTypePing);
  uint8_t flags = 0;
  uint8_t srcNode = 0;
  uint8_t dstNode = kNodeBroadcast;
  uint16_t seq = 0;
  uint16_t payloadLength = 0;
  uint8_t payload[kMaxBusPayload] = {0};
};

// CRC-16/CCITT-FALSE: polynomial 0x1021, init 0xFFFF, no reflection.
uint16_t crc16Ccitt(const uint8_t* data, size_t length);

// Standard COBS encoding (no trailing delimiter, caller appends 0x00).
// Returns the number of bytes written, or 0 when the output buffer is too small.
size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity);

enum class DecodeResult : uint8_t { kOk, kInvalid };

// Decodes one COBS payload (delimiter already removed) into a Frame.
DecodeResult decodeFrame(const uint8_t* cobsData, size_t length, Frame& out);

// Encodes a frame with COBS and appends the 0x00 delimiter.
// Returns the total number of bytes to write on the UART, or 0 on error.
size_t encodeFrame(const Frame& frame, uint8_t* output, size_t capacity);

// Incremental byte stream decoder: feed raw UART bytes, get complete frames.
// Automatically resynchronises on the next delimiter after an error.
class FrameAssembler {
 public:
  enum class Status : uint8_t { kIncomplete, kFrameReady, kError };

  Status push(uint8_t byte, Frame& out);
  void reset();
  uint32_t errorCount() const { return errors_; }
  uint32_t overflowCount() const { return overflows_; }

 private:
  // The assembler stores COBS encoded bytes (delimiter excluded), hence the
  // encoded-frame size and not the raw-frame size.
  uint8_t buffer_[kMaxEncodedFrameSize] = {0};
  size_t length_ = 0;
  uint32_t errors_ = 0;
  uint32_t overflows_ = 0;
};

const char* messageTypeName(uint8_t type);

}  // namespace cell

#endif  // CELL_PROTOCOL_H
