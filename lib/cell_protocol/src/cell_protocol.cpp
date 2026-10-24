#include "cell_protocol.h"

namespace cell {
namespace {

bool cobsDecode(const uint8_t* input,
                size_t length,
                uint8_t* output,
                size_t capacity,
                size_t& outputLength) {
  size_t readIndex = 0;
  size_t writeIndex = 0;
  while (readIndex < length) {
    const uint8_t code = input[readIndex++];
    if (code == 0U) {
      return false;
    }
    for (uint8_t i = 1U; i < code; ++i) {
      if (readIndex >= length || writeIndex >= capacity) {
        return false;
      }
      output[writeIndex++] = input[readIndex++];
    }
    if (code != 0xFFU && readIndex < length) {
      if (writeIndex >= capacity) {
        return false;
      }
      output[writeIndex++] = 0U;
    }
  }
  outputLength = writeIndex;
  return true;
}

}  // namespace

uint16_t crc16Ccitt(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFFU;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t bit = 0U; bit < 8U; ++bit) {
      if ((crc & 0x8000U) != 0U) {
        crc = static_cast<uint16_t>((crc << 1) ^ 0x1021U);
      } else {
        crc = static_cast<uint16_t>(crc << 1);
      }
    }
  }
  return crc;
}

size_t cobsEncode(const uint8_t* input, size_t length, uint8_t* output, size_t capacity) {
  if (output == nullptr || capacity == 0U) {
    return 0U;
  }
  size_t readIndex = 0;
  size_t writeIndex = 1;
  size_t codeIndex = 0;
  uint8_t code = 1U;
  while (readIndex < length) {
    if (input[readIndex] == 0U) {
      if (writeIndex >= capacity) {
        return 0U;
      }
      output[codeIndex] = code;
      code = 1U;
      codeIndex = writeIndex++;
      ++readIndex;
    } else {
      if (writeIndex >= capacity) {
        return 0U;
      }
      output[writeIndex++] = input[readIndex++];
      if (++code == 0xFFU) {
        output[codeIndex] = code;
        code = 1U;
        codeIndex = writeIndex++;
      }
    }
  }
  if (codeIndex >= capacity) {
    return 0U;
  }
  output[codeIndex] = code;
  return writeIndex;
}

size_t encodeFrame(const Frame& frame, uint8_t* output, size_t capacity) {
  if (output == nullptr || frame.payloadLength > kMaxBusPayload) {
    return 0U;
  }
  const size_t rawLength = kFrameHeaderSize + frame.payloadLength + 2U;
  if (capacity < rawLength + 2U) {
    return 0U;
  }

  uint8_t raw[kMaxRawFrameSize] = {0};
  size_t offset = 0;
  raw[offset++] = frame.version;
  raw[offset++] = frame.type;
  raw[offset++] = frame.flags;
  raw[offset++] = frame.srcNode;
  raw[offset++] = frame.dstNode;
  raw[offset++] = static_cast<uint8_t>(frame.seq & 0xFFU);
  raw[offset++] = static_cast<uint8_t>((frame.seq >> 8) & 0xFFU);
  raw[offset++] = static_cast<uint8_t>(frame.payloadLength & 0xFFU);
  raw[offset++] = static_cast<uint8_t>((frame.payloadLength >> 8) & 0xFFU);
  if (frame.payloadLength > 0U) {
    memcpy(raw + offset, frame.payload, frame.payloadLength);
    offset += frame.payloadLength;
  }
  const uint16_t crc = crc16Ccitt(raw, offset);
  raw[offset++] = static_cast<uint8_t>(crc & 0xFFU);
  raw[offset++] = static_cast<uint8_t>((crc >> 8) & 0xFFU);

  const size_t encoded = cobsEncode(raw, offset, output, capacity - 1U);
  if (encoded == 0U) {
    return 0U;
  }
  output[encoded] = 0x00U;
  return encoded + 1U;
}

DecodeResult decodeFrame(const uint8_t* cobsData, size_t length, Frame& out) {
  if (cobsData == nullptr || length == 0U) {
    return DecodeResult::kInvalid;
  }
  uint8_t raw[kMaxRawFrameSize] = {0};
  size_t rawLength = 0;
  if (!cobsDecode(cobsData, length, raw, sizeof(raw), rawLength)) {
    return DecodeResult::kInvalid;
  }
  if (rawLength < kFrameHeaderSize + 2U) {
    return DecodeResult::kInvalid;
  }
  if (raw[0] != kProtocolVersion) {
    return DecodeResult::kInvalid;
  }
  const uint16_t payloadLength =
      static_cast<uint16_t>(raw[7]) | (static_cast<uint16_t>(raw[8]) << 8);
  if (payloadLength > kMaxBusPayload) {
    return DecodeResult::kInvalid;
  }
  const size_t expected = kFrameHeaderSize + payloadLength + 2U;
  if (rawLength != expected) {
    return DecodeResult::kInvalid;
  }
  const uint16_t expectedCrc = static_cast<uint16_t>(raw[expected - 2U]) |
                               (static_cast<uint16_t>(raw[expected - 1U]) << 8);
  if (crc16Ccitt(raw, expected - 2U) != expectedCrc) {
    return DecodeResult::kInvalid;
  }

  out.version = raw[0];
  out.type = raw[1];
  out.flags = raw[2];
  out.srcNode = raw[3];
  out.dstNode = raw[4];
  out.seq = static_cast<uint16_t>(raw[5]) | (static_cast<uint16_t>(raw[6]) << 8);
  out.payloadLength = payloadLength;
  if (payloadLength > 0U) {
    memcpy(out.payload, raw + kFrameHeaderSize, payloadLength);
  }
  return DecodeResult::kOk;
}

FrameAssembler::Status FrameAssembler::push(uint8_t byte, Frame& out) {
  if (byte == 0x00U) {
    if (length_ == 0U) {
      return Status::kIncomplete;
    }
    const size_t encodedLength = length_;
    length_ = 0;
    if (decodeFrame(buffer_, encodedLength, out) == DecodeResult::kOk) {
      return Status::kFrameReady;
    }
    ++errors_;
    return Status::kError;
  }

  if (length_ >= sizeof(buffer_)) {
    ++overflows_;
    ++errors_;
    length_ = 0;
    return Status::kError;
  }
  buffer_[length_++] = byte;
  return Status::kIncomplete;
}

void FrameAssembler::reset() {
  length_ = 0;
}

const char* messageTypeName(uint8_t type) {
  switch (type) {
    case kTypePing: return "PING";
    case kTypePong: return "PONG";
    case kTypeToken: return "TOKEN";
    case kTypeRouteState: return "ROUTE_STATE";
    case kTypeRouteRequest: return "ROUTE_REQUEST";
    case kTypeBusAck: return "BUS_ACK";
    case kTypeText: return "TEXT";
    case kTypeReceipt: return "RECEIPT";
    case kTypeVoiceBlock: return "VOICE_BLOCK";
    case kTypeVoiceFeedback: return "VOICE_FEEDBACK";
    case kTypeAppControl: return "APP_CONTROL";
    default: return "UNKNOWN";
  }
}

}  // namespace cell
