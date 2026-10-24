// Tests of the cross cell message path, for every direction.
//
// The recorded voice notes, the text messages and their receipts all travel on
// the RS485 bus through the same code: RoutingTable (which station hosts the
// destination), buildAppEnvelope (the two user ids), encodeFrame / FrameAssembler
// (the real COBS bus framing) and FragmentAssembler (a payload larger than one
// bus frame).
//
// This file exercises that whole chain, in both directions of every cell pair, so
// that a defect which only affects one direction (A -> B working while B -> A is
// broken) is caught by the suite instead of being discovered in the field.
//
// It is a deterministic simulation of the bus, not a hardware test: the frames go
// through the real encoders and decoders, but the medium itself is not modelled.

#include <unity.h>

#include <cstring>
#include <new>

#include "cell_core.h"

namespace {

constexpr uint8_t kStationA = 1;
constexpr uint8_t kStationB = 2;
constexpr uint8_t kStationC = 3;
// Same value as MeshNode uses to expire a local user that stopped refreshing its
// presence (mesh_node.cpp).
constexpr uint32_t kLocalUserTimeoutMs = 90000U;

// ---------------------------------------------------------------------------
// A station, reduced to what the message path uses.
// ---------------------------------------------------------------------------
struct Station {
  uint8_t nodeId = 0;
  cell::RoutingTable routes;
  cell::FrameAssembler bus;           // receives the raw UART bytes
  cell::FragmentAssembler fragments;  // reassembles a fragmented payload

  // What the local application received.
  uint8_t lastBody[cell::kMaxReassembledPayload] = {0};
  size_t lastLength = 0;
  uint8_t lastType = 0;
  char lastSource[cell::kMaxUserIdLength] = {0};
  char lastDestination[cell::kMaxUserIdLength] = {0};
  uint32_t delivered = 0;
  uint32_t undeliverable = 0;

  void resetInbox() {
    lastLength = 0;
    lastType = 0;
    lastSource[0] = '\0';
    lastDestination[0] = '\0';
    delivered = 0;
    undeliverable = 0;
  }
};

// The receiving station side: reassemble if needed, read the envelope, hand the
// body to the application. Exactly what MeshNode::handleApplicationFrame does.
void deliverFrame(Station& to, const cell::Frame& frame, uint32_t nowMs) {
  uint8_t assembled[cell::kMaxReassembledPayload] = {0};
  size_t assembledLength = 0;
  const uint8_t* payload = frame.payload;
  size_t payloadLength = frame.payloadLength;
  if ((frame.flags & cell::kFlagFragmented) != 0U) {
    const cell::FragmentAssembler::Status status =
        to.fragments.accept(frame, nowMs, assembled, sizeof(assembled), assembledLength);
    if (status != cell::FragmentAssembler::Status::kComplete) {
      return;
    }
    payload = assembled;
    payloadLength = assembledLength;
  }
  cell::AppEnvelope envelope;
  if (!cell::parseAppEnvelope(payload, payloadLength, envelope)) {
    ++to.undeliverable;
    return;
  }
  to.lastType = frame.type;
  memcpy(to.lastSource, envelope.source, sizeof(to.lastSource));
  to.lastSource[sizeof(to.lastSource) - 1U] = '\0';
  memcpy(to.lastDestination, envelope.destination, sizeof(to.lastDestination));
  to.lastDestination[sizeof(to.lastDestination) - 1U] = '\0';
  to.lastLength = envelope.bodyLength;
  if (envelope.bodyLength <= sizeof(to.lastBody)) {
    memcpy(to.lastBody, envelope.body, envelope.bodyLength);
  }
  ++to.delivered;
}

// ---------------------------------------------------------------------------
// The star topology used by the deployment: A is the hub, B and C are leaves. A
// frame between two leaves is relayed by the hub byte for byte.
// ---------------------------------------------------------------------------
struct Deployment {
  Station a;
  Station b;
  Station c;

  Deployment() {
    a.nodeId = kStationA;
    b.nodeId = kStationB;
    c.nodeId = kStationC;
  }

  Station& byId(uint8_t nodeId) {
    return (nodeId == kStationB) ? b : ((nodeId == kStationC) ? c : a);
  }
};

struct SendReport {
  bool accepted = false;
  bool unknownRecipient = false;
  uint8_t destinationStation = 0;
  uint8_t fragmentCount = 1;
  bool relayedByHub = false;
};

// Encodes one frame, pushes it through the hub when the two cells are leaves, and
// delivers it. The hub is a real station here: its FrameAssembler decodes the
// frame again, which proves the relay does not alter the payload.
void transmit(Deployment& deployment,
              uint8_t fromNode,
              uint8_t toNode,
              const cell::Frame& frame,
              SendReport& report,
              uint32_t nowMs) {
  uint8_t encoded[cell::kMaxEncodedFrameSize] = {0};
  const size_t encodedLength = cell::encodeFrame(frame, encoded, sizeof(encoded));
  if (encodedLength == 0U) {
    return;
  }

  cell::Frame received;
  if (fromNode != kStationA && toNode != kStationA) {
    report.relayedByHub = true;
    bool relayed = false;
    for (size_t i = 0; i < encodedLength; ++i) {
      cell::Frame atHub;
      if (deployment.a.bus.push(encoded[i], atHub) != cell::FrameAssembler::Status::kFrameReady) {
        continue;
      }
      // The hub only looks at the destination node: everything else is forwarded
      // unchanged. This is the property that keeps the payload byte identical.
      TEST_ASSERT_EQUAL_UINT8(frame.dstNode, atHub.dstNode);
      TEST_ASSERT_EQUAL_UINT8(frame.type, atHub.type);
      received = atHub;
      relayed = true;
    }
    TEST_ASSERT_TRUE(relayed);
  } else {
    for (size_t i = 0; i < encodedLength; ++i) {
      if (deployment.byId(toNode).bus.push(encoded[i], received) ==
          cell::FrameAssembler::Status::kFrameReady) {
        break;
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT8(frame.dstNode, received.dstNode);
  TEST_ASSERT_EQUAL_UINT16(frame.seq, received.seq);
  TEST_ASSERT_EQUAL_UINT16(frame.payloadLength, received.payloadLength);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(frame.payload, received.payload, frame.payloadLength);
  deliverFrame(deployment.byId(toNode), received, nowMs);
}

// The station side of an outgoing message, as MeshNode::submitAppPayload does it:
// resolve the destination, build the envelope, then send one frame or fragments.
SendReport sendMessage(Deployment& deployment,
                       uint8_t fromNode,
                       const char* srcUser,
                       const char* dstUser,
                       uint8_t type,
                       const uint8_t* body,
                       size_t bodyLength,
                       uint16_t baseSeq) {
  SendReport report;
  Station& from = deployment.byId(fromNode);

  // 1. Which station hosts the recipient right now?
  const cell::RouteEntry* target = from.routes.find(dstUser);
  if (target == nullptr || !target->online) {
    report.unknownRecipient = true;  // refused, never silently lost
    return report;
  }
  report.destinationStation = target->station;

  // 2. The envelope carries the two user ids, so the receiving station can deliver
  //    to the right phone socket without parsing the body.
  uint8_t envelope[cell::kMaxReassembledPayload] = {0};
  const size_t envelopeLength = cell::buildAppEnvelope(srcUser, dstUser, body, bodyLength,
                                                       envelope, sizeof(envelope));
  if (envelopeLength == 0U) {
    return report;
  }

  // 3. One frame when it fits, fragments otherwise.
  cell::Frame frame;
  frame.type = type;
  frame.srcNode = from.nodeId;
  frame.dstNode = target->station;
  frame.seq = baseSeq;

  if (envelopeLength <= cell::kMaxBusPayload) {
    frame.flags = cell::kFlagAckRequired;
    frame.payloadLength = static_cast<uint16_t>(envelopeLength);
    memcpy(frame.payload, envelope, envelopeLength);
    transmit(deployment, fromNode, target->station, frame, report, baseSeq);
    report.accepted = true;
    return report;
  }

  const size_t fragmentCount =
      (envelopeLength + cell::kFragmentBodySize - 1U) / cell::kFragmentBodySize;
  if (fragmentCount > cell::kMaxFragments) {
    return report;
  }
  report.fragmentCount = static_cast<uint8_t>(fragmentCount);
  frame.flags = static_cast<uint8_t>(cell::kFlagAckRequired | cell::kFlagFragmented);
  for (size_t index = 0; index < fragmentCount; ++index) {
    const size_t offset = index * cell::kFragmentBodySize;
    const size_t chunk = ((envelopeLength - offset) < cell::kFragmentBodySize)
                             ? (envelopeLength - offset)
                             : cell::kFragmentBodySize;
    frame.seq = static_cast<uint16_t>(baseSeq + index);
    frame.payload[0] = static_cast<uint8_t>(index);
    frame.payload[1] = static_cast<uint8_t>(fragmentCount);
    frame.payload[2] = static_cast<uint8_t>(baseSeq & 0xFFU);
    frame.payload[3] = static_cast<uint8_t>((baseSeq >> 8U) & 0xFFU);
    memcpy(frame.payload + cell::kFragmentOverhead, envelope + offset, chunk);
    frame.payloadLength = static_cast<uint16_t>(cell::kFragmentOverhead + chunk);
    transmit(deployment, fromNode, target->station, frame, report,
             static_cast<uint32_t>(baseSeq) + static_cast<uint32_t>(index));
  }
  report.accepted = true;
  return report;
}

// A realistic recorded voice block: header + 400 byte payload, as
// AppServer::handleVoiceBlock builds it before handing it to the bus.
constexpr size_t kVoiceBlockSize = 416;

void fillBody(uint8_t* body, size_t length, uint8_t seed) {
  for (size_t i = 0; i < length; ++i) {
    body[i] = static_cast<uint8_t>(seed + i);
  }
}

// The three stations do not fit on the stack of the ESP32 test task (one
// FragmentAssembler already holds two 5.7 kB reassembly contexts), so the
// deployment lives in static storage and is rebuilt for every test.
Deployment& freshDeployment() {
  static Deployment storage;
  storage.~Deployment();
  new (&storage) Deployment();
  return storage;
}

// Registers a user on its station and announces it to the two others, exactly
// like the presence flow of the bus does.
void join(Deployment& deployment,
          uint8_t node,
          const char* user,
          const char* name,
          uint32_t nowMs) {
  Station& station = deployment.byId(node);
  TEST_ASSERT_TRUE(station.routes.upsert(user, name, node, nowMs));
  uint8_t buffer[cell::kMaxBusPayload] = {0};
  const size_t length = station.routes.serializeStationState(node, buffer, sizeof(buffer));
  TEST_ASSERT_TRUE(length > 0U);
  const uint8_t others[] = {kStationA, kStationB, kStationC};
  for (uint8_t other : others) {
    if (other == node) {
      continue;
    }
    TEST_ASSERT_TRUE(deployment.byId(other).routes.applyStationState(node, buffer, length, nowMs));
  }
}

struct Pair {
  uint8_t from;
  uint8_t to;
  const char* fromUser;
  const char* toUser;
};

constexpr Pair kAllPairs[] = {
    {kStationA, kStationB, "u-a1", "u-b1"},
    {kStationB, kStationA, "u-b1", "u-a1"},
    {kStationA, kStationC, "u-a1", "u-c1"},
    {kStationC, kStationA, "u-c1", "u-a1"},
    {kStationB, kStationC, "u-b1", "u-c1"},
    {kStationC, kStationB, "u-c1", "u-b1"},
};

}  // namespace

// Every ordered pair of cells must work, not just one direction: an asymmetric
// routing defect (A can send to B but B cannot answer A) is exactly what this
// loop is meant to catch.
void testMessagingRecordedVoiceReachesEveryCellPair() {
  Deployment& deployment = freshDeployment();
  join(deployment, kStationA, "u-a1", "Alice", 1000U);
  join(deployment, kStationB, "u-b1", "Bob", 1000U);
  join(deployment, kStationC, "u-c1", "Carol", 1000U);

  uint8_t body[kVoiceBlockSize] = {0};
  uint16_t seq = 100U;
  for (const Pair& pair : kAllPairs) {
    Station& destination = deployment.byId(pair.to);
    destination.resetInbox();
    fillBody(body, sizeof(body), static_cast<uint8_t>(seq));

    const SendReport report = sendMessage(deployment, pair.from, pair.fromUser, pair.toUser,
                                          static_cast<uint8_t>(cell::kTypeVoiceBlock), body,
                                          sizeof(body), seq);
    TEST_ASSERT_TRUE_MESSAGE(report.accepted, "the voice block must be accepted");
    TEST_ASSERT_FALSE(report.unknownRecipient);
    TEST_ASSERT_EQUAL_UINT8(pair.to, report.destinationStation);
    TEST_ASSERT_EQUAL_UINT8(1U, report.fragmentCount);
    // Two leaves go through the hub, a pair that includes A does not.
    const bool relayed = (pair.from != kStationA) && (pair.to != kStationA);
    TEST_ASSERT_EQUAL_UINT8(relayed ? 1U : 0U, report.relayedByHub ? 1U : 0U);

    TEST_ASSERT_EQUAL_UINT32(1U, destination.delivered);
    TEST_ASSERT_EQUAL_UINT32(0U, destination.undeliverable);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::kTypeVoiceBlock), destination.lastType);
    TEST_ASSERT_EQUAL_STRING(pair.fromUser, destination.lastSource);
    TEST_ASSERT_EQUAL_STRING(pair.toUser, destination.lastDestination);
    TEST_ASSERT_EQUAL_UINT32(kVoiceBlockSize, destination.lastLength);
    // The body must arrive byte for byte: no re-encoding, no truncation.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(body, destination.lastBody, kVoiceBlockSize);
    seq = static_cast<uint16_t>(seq + 10U);
  }
}

// The same for a text message and for a receipt: they use the same envelope and
// the same bus, so they must cross the six directions too.
void testMessagingTextAndReceiptsReachEveryCellPair() {
  Deployment& deployment = freshDeployment();
  join(deployment, kStationA, "u-a1", "Alice", 1000U);
  join(deployment, kStationB, "u-b1", "Bob", 1000U);
  join(deployment, kStationC, "u-c1", "Carol", 1000U);

  const char* text = "{\"id\":\"m-1\",\"text\":\"bonjour depuis une autre cellule\"}";
  const size_t textLength = strlen(text);
  const char* receipt = "{\"id\":\"m-1\",\"state\":\"delivered\"}";
  const size_t receiptLength = strlen(receipt);

  uint16_t seq = 500U;
  for (const Pair& pair : kAllPairs) {
    Station& destination = deployment.byId(pair.to);
    destination.resetInbox();
    const SendReport report =
        sendMessage(deployment, pair.from, pair.fromUser, pair.toUser,
                    static_cast<uint8_t>(cell::kTypeText),
                    reinterpret_cast<const uint8_t*>(text), textLength, seq);
    TEST_ASSERT_TRUE(report.accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, destination.delivered);
    TEST_ASSERT_EQUAL_UINT32(textLength, destination.lastLength);
    TEST_ASSERT_EQUAL_MEMORY(text, destination.lastBody, textLength);
    seq = static_cast<uint16_t>(seq + 1U);

    // The answer comes back on the same path: this is the direction that was
    // reported as broken in the field.
    const Pair back = {pair.to, pair.from, pair.toUser, pair.fromUser};
    Station& origin = deployment.byId(back.to);
    origin.resetInbox();
    const SendReport receiptReport =
        sendMessage(deployment, back.from, back.fromUser, back.toUser,
                    static_cast<uint8_t>(cell::kTypeReceipt),
                    reinterpret_cast<const uint8_t*>(receipt), receiptLength, seq);
    TEST_ASSERT_TRUE(receiptReport.accepted);
    TEST_ASSERT_EQUAL_UINT32(1U, origin.delivered);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(cell::kTypeReceipt), origin.lastType);
    TEST_ASSERT_EQUAL_UINT32(receiptLength, origin.lastLength);
    TEST_ASSERT_EQUAL_MEMORY(receipt, origin.lastBody, receiptLength);
    seq = static_cast<uint16_t>(seq + 1U);
  }
}

// A user that changes cell must receive its messages on its NEW cell, and the
// messages sent to the old cell must not be delivered there any more.
void testMessagingFollowsAUserThatChangesCell() {
  Deployment& deployment = freshDeployment();
  join(deployment, kStationA, "u-a1", "Alice", 1000U);
  join(deployment, kStationB, "u-b1", "Bob", 1000U);
  join(deployment, kStationC, "u-c1", "Carol", 1000U);

  uint8_t body[kVoiceBlockSize] = {0};
  fillBody(body, sizeof(body), 7U);

  // Bob is on B: the message of Alice reaches B.
  SendReport report = sendMessage(deployment, kStationA, "u-a1", "u-b1",
                                  static_cast<uint8_t>(cell::kTypeVoiceBlock), body,
                                  sizeof(body), 700U);
  TEST_ASSERT_TRUE(report.accepted);
  TEST_ASSERT_EQUAL_UINT8(kStationB, report.destinationStation);
  TEST_ASSERT_EQUAL_UINT32(1U, deployment.b.delivered);

  // Bob moves to C: same identifier, same name, new station.
  deployment.b.resetInbox();
  deployment.c.resetInbox();
  join(deployment, kStationC, "u-b1", "Bob", 2000U);
  TEST_ASSERT_EQUAL_UINT8(kStationC, deployment.a.routes.find("u-b1")->station);

  // Cell B notices that Bob stopped refreshing its presence and stops announcing
  // him: without this, B would keep claiming a user it no longer hosts.
  deployment.b.routes.expireLocal(kStationB, 2000U + kLocalUserTimeoutMs, kLocalUserTimeoutMs);
  uint8_t buffer[cell::kMaxBusPayload] = {0};
  const size_t length =
      deployment.b.routes.serializeStationState(kStationB, buffer, sizeof(buffer));
  TEST_ASSERT_TRUE(length > 0U);
  TEST_ASSERT_TRUE(deployment.a.routes.applyStationState(kStationB, buffer, length, 2100U));

  // The identity of Bob is unique on every cell, and it points at C now.
  TEST_ASSERT_EQUAL_UINT32(3U, static_cast<uint32_t>(deployment.a.routes.entries()));
  TEST_ASSERT_EQUAL_UINT8(kStationC, deployment.a.routes.find("u-b1")->station);
  TEST_ASSERT_EQUAL_STRING("Bob", deployment.a.routes.find("u-b1")->displayName);

  // The next message is routed to C, and B receives nothing.
  report = sendMessage(deployment, kStationA, "u-a1", "u-b1",
                       static_cast<uint8_t>(cell::kTypeVoiceBlock), body, sizeof(body), 800U);
  TEST_ASSERT_TRUE(report.accepted);
  TEST_ASSERT_EQUAL_UINT8(kStationC, report.destinationStation);
  TEST_ASSERT_EQUAL_UINT32(0U, deployment.b.delivered);
  TEST_ASSERT_EQUAL_UINT32(1U, deployment.c.delivered);
  TEST_ASSERT_EQUAL_STRING("u-b1", deployment.c.lastDestination);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(body, deployment.c.lastBody, kVoiceBlockSize);
  // The identity itself never changed.
  TEST_ASSERT_EQUAL_STRING("Bob", deployment.a.routes.find("u-b1")->displayName);
}

// An unknown or offline recipient must be refused explicitly: a message must never
// be sent to a station that does not host the recipient, because it would be lost
// there without any error.
void testMessagingUnknownRecipientIsRefusedExplicitly() {
  Deployment& deployment = freshDeployment();
  join(deployment, kStationA, "u-a1", "Alice", 1000U);
  join(deployment, kStationB, "u-b1", "Bob", 1000U);

  uint8_t body[kVoiceBlockSize] = {0};
  fillBody(body, sizeof(body), 9U);

  // A user that was never announced.
  SendReport report = sendMessage(deployment, kStationA, "u-a1", "u-ghost",
                                  static_cast<uint8_t>(cell::kTypeVoiceBlock), body,
                                  sizeof(body), 900U);
  TEST_ASSERT_FALSE(report.accepted);
  TEST_ASSERT_TRUE(report.unknownRecipient);
  TEST_ASSERT_EQUAL_UINT32(0U, deployment.b.delivered);
  TEST_ASSERT_EQUAL_UINT32(0U, deployment.c.delivered);

  // A user that is known but offline.
  TEST_ASSERT_TRUE(deployment.a.routes.setOnline("u-b1", false));
  report = sendMessage(deployment, kStationA, "u-a1", "u-b1",
                       static_cast<uint8_t>(cell::kTypeVoiceBlock), body, sizeof(body), 901U);
  TEST_ASSERT_FALSE(report.accepted);
  TEST_ASSERT_TRUE(report.unknownRecipient);
  TEST_ASSERT_EQUAL_UINT32(0U, deployment.b.delivered);
}

// A payload larger than one bus frame is fragmented and reassembled: the received
// body must still be byte identical.
void testMessagingFragmentedPayloadIsReassembled() {
  Deployment& deployment = freshDeployment();
  join(deployment, kStationA, "u-a1", "Alice", 1000U);
  join(deployment, kStationB, "u-b1", "Bob", 1000U);

  // 1200 bytes of body plus the envelope exceed the 480 byte bus payload.
  uint8_t body[1200] = {0};
  fillBody(body, sizeof(body), 3U);

  deployment.b.resetInbox();
  const SendReport report = sendMessage(deployment, kStationA, "u-a1", "u-b1",
                                        static_cast<uint8_t>(cell::kTypeVoiceBlock), body,
                                        sizeof(body), 1000U);
  TEST_ASSERT_TRUE(report.accepted);
  TEST_ASSERT_TRUE(report.fragmentCount > 1U);
  TEST_ASSERT_EQUAL_UINT32(1U, deployment.b.delivered);
  TEST_ASSERT_EQUAL_UINT32(sizeof(body), deployment.b.lastLength);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(body, deployment.b.lastBody, sizeof(body));
}

