#include <unity.h>
#include <cstring>

#include "cell_protocol.h"

namespace {

cell::Frame makeFrame(uint8_t type,
                      uint8_t src,
                      uint8_t dst,
                      uint16_t seq,
                      const uint8_t* payload,
                      size_t payloadLength) {
  cell::Frame frame;
  frame.type = type;
  frame.srcNode = src;
  frame.dstNode = dst;
  frame.seq = seq;
  frame.payloadLength = static_cast<uint16_t>(payloadLength);
  if (payload != nullptr && payloadLength > 0U) {
    memcpy(frame.payload, payload, payloadLength);
  }
  return frame;
}

}  // namespace

void testProtocolRoundtrip() {
  const uint8_t payload[] = {'H', 'E', 'L', 'L', 'O', 0x00, 'A', 0xFF};
  const cell::Frame source = makeFrame(cell::kTypeText, 1, 3, 4242, payload, sizeof(payload));

  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  const size_t length = cell::encodeFrame(source, encoded, sizeof(encoded));
  TEST_ASSERT_GREATER_THAN_UINT32(0U, static_cast<uint32_t>(length));
  TEST_ASSERT_EQUAL_UINT8(0x00U, encoded[length - 1U]);

  cell::Frame decoded;
  const cell::DecodeResult result = cell::decodeFrame(encoded, length - 1U, decoded);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::DecodeResult::kOk),
                          static_cast<uint8_t>(result));
  TEST_ASSERT_EQUAL_UINT8(cell::kTypeText, decoded.type);
  TEST_ASSERT_EQUAL_UINT8(1, decoded.srcNode);
  TEST_ASSERT_EQUAL_UINT8(3, decoded.dstNode);
  TEST_ASSERT_EQUAL_UINT16(4242, decoded.seq);
  TEST_ASSERT_EQUAL_UINT16(sizeof(payload), decoded.payloadLength);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, decoded.payload, sizeof(payload));
}

void testProtocolCrcRejectsCorruption() {
  const uint8_t payload[] = {'b', 'o', 'n', 'j', 'o', 'u', 'r'};
  const cell::Frame source = makeFrame(cell::kTypeText, 2, 1, 7, payload, sizeof(payload));

  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  const size_t length = cell::encodeFrame(source, encoded, sizeof(encoded));
  TEST_ASSERT_GREATER_THAN_UINT32(0U, static_cast<uint32_t>(length));

  encoded[2] ^= 0x5AU;  // flip a bit inside the payload area

  cell::Frame decoded;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::DecodeResult::kInvalid),
                          static_cast<uint8_t>(cell::decodeFrame(encoded, length - 1U, decoded)));
}

void testProtocolRejectsTruncatedFrame() {
  const uint8_t payload[] = {'x', 'y', 'z'};
  const cell::Frame source = makeFrame(cell::kTypePing, 1, 0, 1, payload, sizeof(payload));

  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  const size_t length = cell::encodeFrame(source, encoded, sizeof(encoded));
  TEST_ASSERT_GREATER_THAN_UINT32(3U, static_cast<uint32_t>(length));

  cell::Frame decoded;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::DecodeResult::kInvalid),
                          static_cast<uint8_t>(cell::decodeFrame(encoded, length - 3U, decoded)));
}

void testProtocolRejectsOversizedPayload() {
  cell::Frame frame = makeFrame(cell::kTypeText, 1, 2, 1, nullptr, 0);
  frame.payloadLength = static_cast<uint16_t>(cell::kMaxBusPayload + 1U);

  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(cell::encodeFrame(frame, encoded,
                                                                      sizeof(encoded))));
}

void testAssemblerResynchronisesAfterGarbage() {
  const uint8_t payload[] = {'o', 'k'};
  const cell::Frame source = makeFrame(cell::kTypeReceipt, 3, 1, 99, payload, sizeof(payload));

  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  const size_t length = cell::encodeFrame(source, encoded, sizeof(encoded));

  cell::FrameAssembler assembler;
  cell::Frame decoded;

  // Corrupted frame: the delimiter must close it and report an error.
  const uint8_t garbage[] = {0x41, 0x42, 0x00};
  cell::FrameAssembler::Status status = cell::FrameAssembler::Status::kIncomplete;
  for (size_t i = 0; i < sizeof(garbage); ++i) {
    status = assembler.push(garbage[i], decoded);
  }
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FrameAssembler::Status::kError),
                          static_cast<uint8_t>(status));
  TEST_ASSERT_EQUAL_UINT32(1U, assembler.errorCount());

  // The next valid frame must still be decoded.
  for (size_t i = 0; i < length; ++i) {
    status = assembler.push(encoded[i], decoded);
  }
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::FrameAssembler::Status::kFrameReady),
                          static_cast<uint8_t>(status));
  TEST_ASSERT_EQUAL_STRING("ok", reinterpret_cast<const char*>(decoded.payload));
  TEST_ASSERT_EQUAL_UINT8(3, decoded.srcNode);
}

void testAssemblerHandlesByteByByteStream() {
  uint8_t payload[cell::kMaxBusPayload];
  for (size_t i = 0; i < sizeof(payload); ++i) {
    payload[i] = static_cast<uint8_t>(i * 7U);
  }
  const cell::Frame source = makeFrame(cell::kTypeVoiceBlock, 2, 3, 60000, payload, sizeof(payload));

  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  const size_t length = cell::encodeFrame(source, encoded, sizeof(encoded));
  TEST_ASSERT_GREATER_THAN_UINT32(0U, static_cast<uint32_t>(length));

  cell::FrameAssembler assembler;
  cell::Frame decoded;
  size_t frames = 0;
  for (size_t i = 0; i < length; ++i) {
    if (assembler.push(encoded[i], decoded) == cell::FrameAssembler::Status::kFrameReady) {
      ++frames;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(frames));
  TEST_ASSERT_EQUAL_UINT16(cell::kMaxBusPayload, decoded.payloadLength);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, decoded.payload, sizeof(payload));
  TEST_ASSERT_EQUAL_UINT32(0U, assembler.errorCount());
}

void testProtocolTypeNames() {
  TEST_ASSERT_EQUAL_STRING("TEXT", cell::messageTypeName(cell::kTypeText));
  TEST_ASSERT_EQUAL_STRING("TOKEN", cell::messageTypeName(cell::kTypeToken));
  TEST_ASSERT_EQUAL_STRING("VOICE_BLOCK", cell::messageTypeName(cell::kTypeVoiceBlock));
  TEST_ASSERT_EQUAL_STRING("UNKNOWN", cell::messageTypeName(0xEE));
}
