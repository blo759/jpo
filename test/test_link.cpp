// Tests of the inter-station IP link (lib/cell_link) and of the Wi-Fi backbone
// glue (src/common/station_link.*).
//
// Two families of checks:
//   * behaviour: binary framing, stream reassembly, authentication, hop limited
//     routing (star and chain), reconnection policy, liveness,
//   * structural guards proving that the backbone is not the RS485 bus: neither
//     the link module nor the station glue may reference the mesh, the bus frame
//     types or the protocol library.

#include <unity.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "cell_link.h"
#include "cell_protocol.h"

namespace {

std::string readSource(const char* relativePath) {
  const char* prefixes[] = {"", "../", "../../", "../../../"};
  for (const char* prefix : prefixes) {
    std::ifstream stream(std::string(prefix) + relativePath);
    if (!stream.good()) {
      continue;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
  }
  return std::string();
}

bool contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

cell::LinkFrame makeFrame(uint8_t type, uint8_t src, uint8_t dst, const char* text) {
  cell::LinkFrame frame;
  frame.type = type;
  frame.srcNode = src;
  frame.dstNode = dst;
  frame.hops = 0;
  const size_t length = text == nullptr ? 0U : strlen(text);
  if (length > 0U) {
    memcpy(frame.payload, text, length);
    frame.payloadLength = static_cast<uint16_t>(length);
  }
  return frame;
}

// Feeds a whole encoded frame to a stream assembler, byte by byte, and returns
// the number of complete frames it produced.
size_t feedAll(const uint8_t* data, size_t length, cell::LinkFrame& last) {
  cell::LinkStreamAssembler assembler;
  size_t ready = 0;
  for (size_t i = 0; i < length; ++i) {
    cell::LinkFrame frame;
    if (assembler.push(data[i], frame) == cell::LinkStreamAssembler::Status::kFrameReady) {
      last = frame;
      ++ready;
    }
  }
  return ready;
}

}  // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------
void testLinkFrameRoundTrip() {
  const cell::LinkFrame original = makeFrame(cell::kLinkCallSignal, 2U, 3U, "payload");
  uint8_t encoded[cell::kLinkMaxFrameSize] = {0};
  const size_t length = cell::encodeLinkFrame(original, encoded, sizeof(encoded));
  TEST_ASSERT_EQUAL_UINT32(
      static_cast<uint32_t>(cell::kLinkHeaderSize + original.payloadLength),
      static_cast<uint32_t>(length));
  TEST_ASSERT_EQUAL_UINT8(cell::kLinkMagic, encoded[0]);
  TEST_ASSERT_EQUAL_UINT8(cell::kLinkVersion, encoded[1]);

  cell::LinkFrame decoded;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kOk),
                    static_cast<int>(cell::decodeLinkFrame(encoded, length, decoded)));
  TEST_ASSERT_EQUAL_UINT8(cell::kLinkCallSignal, decoded.type);
  TEST_ASSERT_EQUAL_UINT8(2U, decoded.srcNode);
  TEST_ASSERT_EQUAL_UINT8(3U, decoded.dstNode);
  TEST_ASSERT_EQUAL_UINT16(original.payloadLength, decoded.payloadLength);
  TEST_ASSERT_EQUAL_INT(0, memcmp(decoded.payload, original.payload, original.payloadLength));
}

void testLinkFrameRejectsMalformedInput() {
  const cell::LinkFrame original = makeFrame(cell::kLinkHeartbeat, 1U, 2U, "x");
  uint8_t encoded[cell::kLinkMaxFrameSize] = {0};
  const size_t length = cell::encodeLinkFrame(original, encoded, sizeof(encoded));
  TEST_ASSERT_TRUE(length > 0U);

  cell::LinkFrame decoded;
  uint8_t broken[cell::kLinkMaxFrameSize];

  // Wrong magic.
  memcpy(broken, encoded, length);
  broken[0] = 0x00U;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kInvalid),
                    static_cast<int>(cell::decodeLinkFrame(broken, length, decoded)));

  // Wrong version.
  memcpy(broken, encoded, length);
  broken[1] = 0x7FU;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kInvalid),
                    static_cast<int>(cell::decodeLinkFrame(broken, length, decoded)));

  // Declared length larger than the buffer.
  memcpy(broken, encoded, length);
  broken[8] = 0xFFU;
  broken[9] = 0x00U;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kInvalid),
                    static_cast<int>(cell::decodeLinkFrame(broken, length, decoded)));

  // Declared length smaller than the buffer.
  memcpy(broken, encoded, length);
  broken[8] = 0x00U;
  broken[9] = 0x00U;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kInvalid),
                    static_cast<int>(cell::decodeLinkFrame(broken, length, decoded)));

  // A station can never be its own source (srcNode 0).
  memcpy(broken, encoded, length);
  broken[4] = 0x00U;
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kInvalid),
                    static_cast<int>(cell::decodeLinkFrame(broken, length, decoded)));

  // Truncated header.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kInvalid),
                    static_cast<int>(cell::decodeLinkFrame(encoded,
                                                           cell::kLinkHeaderSize - 1U, decoded)));
  // The valid frame still decodes after all those refusals.
  TEST_ASSERT_EQUAL(static_cast<int>(cell::LinkDecodeResult::kOk),
                    static_cast<int>(cell::decodeLinkFrame(encoded, length, decoded)));
}

void testLinkStreamAssemblerRebuildsFramesFromAStream() {
  const cell::LinkFrame first = makeFrame(cell::kLinkHeartbeat, 1U, 2U, "abc");
  const cell::LinkFrame second = makeFrame(cell::kLinkCallAudio, 2U, 1U, "defgh");
  uint8_t encoded[2U * cell::kLinkMaxFrameSize] = {0};
  const size_t firstLength = cell::encodeLinkFrame(first, encoded, sizeof(encoded));
  const size_t secondLength =
      cell::encodeLinkFrame(second, encoded + firstLength, sizeof(encoded) - firstLength);
  TEST_ASSERT_TRUE(firstLength > 0U && secondLength > 0U);

  cell::LinkFrame last;
  TEST_ASSERT_EQUAL_UINT32(
      2U, static_cast<uint32_t>(feedAll(encoded, firstLength + secondLength, last)));
  TEST_ASSERT_EQUAL_UINT8(cell::kLinkCallAudio, last.type);
  TEST_ASSERT_EQUAL_UINT16(second.payloadLength, last.payloadLength);
}

void testLinkStreamAssemblerResynchronisesAfterGarbage() {
  const cell::LinkFrame frame = makeFrame(cell::kLinkCallSignal, 3U, 1U, "hello");
  uint8_t encoded[cell::kLinkMaxFrameSize] = {0};
  const size_t length = cell::encodeLinkFrame(frame, encoded, sizeof(encoded));

  cell::LinkStreamAssembler assembler;
  // A full header worth of bytes that cannot start a frame: the assembler only
  // discovers it once it has the 12 header bytes, then drops them and starts
  // again from the next byte.
  cell::LinkFrame decoded;
  for (size_t i = 0; i < cell::kLinkHeaderSize; ++i) {
    assembler.push(0x11U, decoded);
  }
  TEST_ASSERT_TRUE(assembler.errors() >= 1U);
  TEST_ASSERT_EQUAL_UINT32(0U, static_cast<uint32_t>(assembler.buffered()));

  size_t ready = 0;
  for (size_t i = 0; i < length; ++i) {
    if (assembler.push(encoded[i], decoded) == cell::LinkStreamAssembler::Status::kFrameReady) {
      ++ready;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(1U, static_cast<uint32_t>(ready));
  TEST_ASSERT_EQUAL_UINT8(3U, decoded.srcNode);
}

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------
void testLinkHelloRoundTripAndTokenCheck() {
  const char* token = "cellnet-demo-link-token";
  uint8_t payload[cell::kLinkHelloPayloadSize] = {0};
  TEST_ASSERT_EQUAL_UINT32(
      static_cast<uint32_t>(cell::kLinkHelloPayloadSize),
      static_cast<uint32_t>(cell::buildLinkHello(2U, token, payload, sizeof(payload))));

  uint8_t nodeId = 0;
  char received[cell::kLinkTokenLength] = {0};
  TEST_ASSERT_TRUE(
      cell::parseLinkHello(payload, sizeof(payload), nodeId, received, sizeof(received)));
  TEST_ASSERT_EQUAL_UINT8(2U, nodeId);
  TEST_ASSERT_EQUAL_STRING(token, received);
  TEST_ASSERT_TRUE(cell::linkTokenMatches(token, strlen(token), received));
  TEST_ASSERT_FALSE(cell::linkTokenMatches(token, strlen(token), "wrong-token"));
  TEST_ASSERT_FALSE(cell::linkTokenMatches(token, strlen(token), ""));
  TEST_ASSERT_FALSE(cell::linkTokenMatches(nullptr, 0U, received));
  TEST_ASSERT_FALSE(cell::linkTokenMatches(token, strlen(token), nullptr));

  // A hello without a token, or with node 0, is refused.
  uint8_t broken[cell::kLinkHelloPayloadSize] = {0};
  TEST_ASSERT_EQUAL_UINT32(
      0U, static_cast<uint32_t>(cell::buildLinkHello(0U, token, broken, sizeof(broken))));
  TEST_ASSERT_FALSE(
      cell::parseLinkHello(broken, sizeof(broken), nodeId, received, sizeof(received)));
  TEST_ASSERT_FALSE(cell::parseLinkHello(payload, sizeof(payload) - 1U, nodeId, received,
                                         sizeof(received)));
}

void testLinkTokenComparisonIsExact() {
  const char* expected = "abcdefgh";
  TEST_ASSERT_TRUE(cell::linkTokenMatches(expected, 8U, "abcdefgh"));
  // A prefix of the expected token is not the token.
  TEST_ASSERT_FALSE(cell::linkTokenMatches(expected, 8U, "abcdefg"));
  // A longer value is not the token either.
  TEST_ASSERT_FALSE(cell::linkTokenMatches(expected, 8U, "abcdefghi"));
  // Same length, one different character.
  TEST_ASSERT_FALSE(cell::linkTokenMatches(expected, 8U, "abcdefgX"));
  // An empty expected token never matches.
  TEST_ASSERT_FALSE(cell::linkTokenMatches("", 0U, ""));
}

// ---------------------------------------------------------------------------
// Call signalling payload
// ---------------------------------------------------------------------------
void testLinkCallSignalRoundTrip() {
  cell::LinkCallSignal signal;
  memcpy(signal.callId, "call0001", 8);
  signal.action = cell::kSignalInvite;
  signal.reason = 0U;
  memcpy(signal.from, "u-a1", 4);
  memcpy(signal.to, "u-b1", 4);

  uint8_t payload[cell::kLinkCallSignalSize] = {0};
  TEST_ASSERT_EQUAL_UINT32(
      static_cast<uint32_t>(cell::kLinkCallSignalSize),
      static_cast<uint32_t>(cell::buildLinkCallSignal(signal, payload, sizeof(payload))));
  cell::LinkCallSignal parsed;
  TEST_ASSERT_TRUE(cell::parseLinkCallSignal(payload, sizeof(payload), parsed));
  TEST_ASSERT_EQUAL_STRING("call0001", parsed.callId);
  TEST_ASSERT_EQUAL_UINT8(cell::kSignalInvite, parsed.action);
  TEST_ASSERT_EQUAL_STRING("u-a1", parsed.from);
  TEST_ASSERT_EQUAL_STRING("u-b1", parsed.to);
}

void testLinkCallSignalRejectsMalformedInput() {
  cell::LinkCallSignal signal;
  memcpy(signal.callId, "call0001", 8);
  signal.action = cell::kSignalAccept;
  memcpy(signal.from, "u-a1", 4);
  memcpy(signal.to, "u-b1", 4);

  uint8_t payload[cell::kLinkCallSignalSize] = {0};
  TEST_ASSERT_TRUE(cell::buildLinkCallSignal(signal, payload, sizeof(payload)) > 0U);

  cell::LinkCallSignal parsed;
  // Wrong size.
  TEST_ASSERT_FALSE(cell::parseLinkCallSignal(payload, sizeof(payload) - 1U, parsed));
  // Unknown action.
  uint8_t broken[cell::kLinkCallSignalSize];
  memcpy(broken, payload, sizeof(payload));
  broken[8] = 0x7FU;
  TEST_ASSERT_FALSE(cell::parseLinkCallSignal(broken, sizeof(broken), parsed));
  // Action 0 is not a signal.
  memcpy(broken, payload, sizeof(payload));
  broken[8] = 0x00U;
  TEST_ASSERT_FALSE(cell::parseLinkCallSignal(broken, sizeof(broken), parsed));
  // A user field without terminator is refused.
  memcpy(broken, payload, sizeof(payload));
  for (size_t i = 10U; i < 34U; ++i) {
    broken[i] = 'x';
  }
  TEST_ASSERT_FALSE(cell::parseLinkCallSignal(broken, sizeof(broken), parsed));
  // The builder refuses an invalid call id, an unknown action and an empty user.
  cell::LinkCallSignal bad = signal;
  bad.callId[0] = ' ';
  TEST_ASSERT_EQUAL_UINT32(
      0U, static_cast<uint32_t>(cell::buildLinkCallSignal(bad, payload, sizeof(payload))));
  bad = signal;
  bad.action = 0x00U;
  TEST_ASSERT_EQUAL_UINT32(
      0U, static_cast<uint32_t>(cell::buildLinkCallSignal(bad, payload, sizeof(payload))));
  bad = signal;
  bad.to[0] = '\0';
  TEST_ASSERT_EQUAL_UINT32(
      0U, static_cast<uint32_t>(cell::buildLinkCallSignal(bad, payload, sizeof(payload))));
}

// ---------------------------------------------------------------------------
// Hop limited routing
// ---------------------------------------------------------------------------
void testLinkRoutesStarTopologyServesEveryCellPair() {
  // The default topology: SITE_A is the hub, SITE_B and SITE_C attach to it.
  cell::LinkRoutes siteA;
  siteA.begin(1U);
  siteA.setDirect(2U, true);
  siteA.setDirect(3U, true);

  cell::LinkRoutes siteB;
  siteB.begin(2U);
  siteB.setUpstream(1U);
  siteB.setDirect(1U, true);

  cell::LinkRoutes siteC;
  siteC.begin(3U);
  siteC.setUpstream(1U);
  siteC.setDirect(1U, true);

  // Same cell: delivered locally, no link hop.
  TEST_ASSERT_EQUAL_UINT8(0U, siteA.nextHop(1U));
  TEST_ASSERT_EQUAL_UINT8(0U, siteB.nextHop(2U));
  TEST_ASSERT_EQUAL_UINT8(0U, siteC.nextHop(3U));

  // A -> B and A -> C are direct; B -> A and C -> A use the upstream station.
  TEST_ASSERT_EQUAL_UINT8(2U, siteA.nextHop(2U));
  TEST_ASSERT_EQUAL_UINT8(3U, siteA.nextHop(3U));
  TEST_ASSERT_EQUAL_UINT8(1U, siteB.nextHop(1U));
  TEST_ASSERT_EQUAL_UINT8(1U, siteC.nextHop(1U));
  // B -> C and C -> B are relayed by the hub.
  TEST_ASSERT_EQUAL_UINT8(1U, siteB.nextHop(3U));
  TEST_ASSERT_EQUAL_UINT8(1U, siteC.nextHop(2U));

  for (uint8_t dst = 1U; dst <= 3U; ++dst) {
    TEST_ASSERT_TRUE(siteA.reachable(dst));
    TEST_ASSERT_TRUE(siteB.reachable(dst));
    TEST_ASSERT_TRUE(siteC.reachable(dst));
  }
}

void testLinkRoutesChainTopologyConvergesThroughAnnouncements() {
  // Chain: SITE_C attaches to SITE_B, SITE_B attaches to SITE_A. SITE_A has no
  // direct socket to SITE_C, so it must learn the route from the announcement.
  cell::LinkRoutes siteA;
  siteA.begin(1U);
  siteA.setDirect(2U, true);
  // Before the announcement, C is not reachable from A.
  TEST_ASSERT_FALSE(siteA.reachable(3U));

  // SITE_B announces that it reaches itself, A and C (mask 0b0111).
  siteA.applyReachableMask(0x07U, 2U);
  TEST_ASSERT_EQUAL_UINT8(2U, siteA.nextHop(3U));
  TEST_ASSERT_TRUE(siteA.reachable(3U));
  TEST_ASSERT_EQUAL_UINT8(2U, siteA.viaOf(3U));
  // The mask never overrides a direct route.
  TEST_ASSERT_EQUAL_UINT8(2U, siteA.nextHop(2U));

  // The other end of the chain: SITE_C knows SITE_B directly and SITE_A through
  // its upstream station.
  cell::LinkRoutes siteC;
  siteC.begin(3U);
  siteC.setUpstream(2U);
  siteC.setDirect(2U, true);
  TEST_ASSERT_EQUAL_UINT8(2U, siteC.nextHop(1U));
  TEST_ASSERT_EQUAL_UINT8(2U, siteC.nextHop(2U));
  TEST_ASSERT_TRUE(siteC.reachable(1U));
}

void testLinkRoutesRelayNeverLoopsBack() {
  cell::LinkRoutes hub;
  hub.begin(1U);
  hub.setDirect(2U, true);
  hub.setDirect(3U, true);

  // A frame for node 3 arriving from node 2 is forwarded to node 3.
  TEST_ASSERT_EQUAL_UINT8(3U, hub.relayTarget(3U, 2U, 0U));
  // The same frame arriving from node 3 is addressed to that node: dropped.
  TEST_ASSERT_EQUAL_UINT8(0U, hub.relayTarget(3U, 3U, 1U));
  // A frame for the hub itself is never relayed.
  TEST_ASSERT_EQUAL_UINT8(0U, hub.relayTarget(1U, 2U, 0U));
  // A frame for an unknown node has no route.
  TEST_ASSERT_EQUAL_UINT8(0U, hub.relayTarget(5U, 2U, 0U));
}

void testLinkRoutesHopLimitDropsTheFrame() {
  cell::LinkRoutes hub;
  hub.begin(1U);
  hub.setDirect(3U, true);
  // One relay is enough for the star; a frame that already crossed the maximum
  // number of stations is dropped instead of circulating for ever.
  TEST_ASSERT_EQUAL_UINT8(3U, hub.relayTarget(3U, 2U, 0U));
  TEST_ASSERT_EQUAL_UINT8(3U, hub.relayTarget(3U, 2U, cell::kLinkMaxHops - 1U));
  TEST_ASSERT_EQUAL_UINT8(0U, hub.relayTarget(3U, 2U, cell::kLinkMaxHops));
  TEST_ASSERT_EQUAL_UINT8(0U, hub.relayTarget(3U, 2U, 200U));
}

void testLinkRoutesForgetTheLearnedPathAfterALinkLoss() {
  cell::LinkRoutes siteA;
  siteA.begin(1U);
  siteA.setDirect(2U, true);
  siteA.applyReachableMask(0x07U, 2U);
  TEST_ASSERT_TRUE(siteA.reachable(3U));

  // SITE_B drops: its direct route and everything learned through it disappear,
  // which is what prevents a black hole.
  siteA.setDirect(2U, false);
  siteA.forgetVia(2U);
  TEST_ASSERT_FALSE(siteA.reachable(3U));
  TEST_ASSERT_FALSE(siteA.reachable(2U));
  TEST_ASSERT_TRUE(siteA.reachable(1U));
}

// ---------------------------------------------------------------------------
// Link health
// ---------------------------------------------------------------------------
void testLinkClientStateReconnectsWithDelay() {
  cell::LinkClientState state;
  state.begin(1000U, 2000U);
  TEST_ASSERT_FALSE(state.connected());
  TEST_ASSERT_TRUE(state.shouldAttempt(1000U));
  state.noteAttempt(1000U);
  TEST_ASSERT_EQUAL_UINT32(1U, state.attempts());
  // The next attempt is only allowed after the retry delay.
  TEST_ASSERT_FALSE(state.shouldAttempt(1500U));
  TEST_ASSERT_TRUE(state.shouldAttempt(3000U));

  state.onConnected(3000U);
  TEST_ASSERT_TRUE(state.connected());
  TEST_ASSERT_FALSE(state.shouldAttempt(99999U));
  TEST_ASSERT_EQUAL_UINT32(1U, state.reconnects());

  state.onDisconnected(5000U);
  TEST_ASSERT_FALSE(state.connected());
  TEST_ASSERT_FALSE(state.shouldAttempt(6000U));
  TEST_ASSERT_TRUE(state.shouldAttempt(7000U));
  state.onConnected(7000U);
  TEST_ASSERT_EQUAL_UINT32(2U, state.reconnects());

  // A zero retry delay is replaced by a sane default.
  cell::LinkClientState other;
  other.begin(0U, 0U);
  TEST_ASSERT_TRUE(other.retryDelayMs() >= 1000U);
}

void testLinkLivenessDetectsASilentPeer() {
  cell::LinkLiveness liveness;
  liveness.begin(1000U);
  TEST_ASSERT_FALSE(liveness.stale(1000U + 15000U, 15000U));
  TEST_ASSERT_TRUE(liveness.stale(1000U + 15001U, 15000U));
  liveness.noteActivity(9000U);
  TEST_ASSERT_FALSE(liveness.stale(9000U + 15000U, 15000U));
  TEST_ASSERT_TRUE(liveness.stale(9000U + 15001U, 15000U));
  TEST_ASSERT_EQUAL_UINT32(9000U, liveness.lastActivityMs());
}

// ---------------------------------------------------------------------------
// Structural guards: the backbone is not the RS485 bus
// ---------------------------------------------------------------------------
void testLinkModuleDoesNotDependOnTheBusLibraries() {
  const std::string header = readSource("lib/cell_link/include/cell_link.h");
  const std::string source = readSource("lib/cell_link/src/cell_link.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "cell_link.h not found from the test working dir");
  TEST_ASSERT_TRUE_MESSAGE(!source.empty(), "cell_link.cpp not found from the test working dir");

  const char* includes[] = {"#include \"cell_protocol.h\"", "#include <cell_protocol.h>",
                            "#include \"cell_core.h\"", "#include <cell_core.h>",
                            "#include \"mesh_node.h\"", "#include \"station_link.h\""};
  for (const char* directive : includes) {
    TEST_ASSERT_FALSE_MESSAGE(contains(header, directive), directive);
    TEST_ASSERT_FALSE_MESSAGE(contains(source, directive), directive);
  }

  const char* forbidden[] = {"FrameQueue", "BusArbiter", "AckTracker", "submitAppPayload",
                             "kTypeText",  "kTypeVoiceBlock", "cobsEncode", "crc16Ccitt",
                             "MeshNode"};
  for (const char* needle : forbidden) {
    TEST_ASSERT_FALSE_MESSAGE(contains(header, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(source, needle), needle);
  }
}

void testStationLinkGlueNeverUsesTheMesh() {
  const std::string header = readSource("src/common/station_link.h");
  const std::string source = readSource("src/common/station_link.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "station_link.h not found");
  TEST_ASSERT_TRUE_MESSAGE(!source.empty(), "station_link.cpp not found");

  const char* forbidden[] = {"mesh_node.h", "cell_core.h", "cell_protocol.h", "MeshNode",
                             "submitAppPayload", "FrameQueue", "BusArbiter", "AckTracker",
                             "kTypeText", "kTypeVoiceBlock", "mesh_", "queue_"};
  for (const char* needle : forbidden) {
    TEST_ASSERT_FALSE_MESSAGE(contains(header, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(source, needle), needle);
  }
}

void testWifiBackboneGlueNeverUsesTheMesh() {
  const std::string header = readSource("src/common/wifi_backbone.h");
  const std::string source = readSource("src/common/wifi_backbone.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!header.empty(), "wifi_backbone.h not found");
  TEST_ASSERT_TRUE_MESSAGE(!source.empty(), "wifi_backbone.cpp not found");

  const char* forbidden[] = {"mesh_node.h", "cell_core.h", "cell_protocol.h", "MeshNode",
                             "submitAppPayload", "FrameQueue", "kTypeText", "kTypeVoiceBlock"};
  for (const char* needle : forbidden) {
    TEST_ASSERT_FALSE_MESSAGE(contains(header, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(source, needle), needle);
  }
}

void testTheBusProtocolAndTheMeshWereNotTouchedByTheBackbone() {
  const std::string protocol = readSource("lib/cell_protocol/include/cell_protocol.h");
  const std::string nodeHeader = readSource("src/common/mesh_node.h");
  const std::string nodeSource = readSource("src/common/mesh_node.cpp");
  const std::string server = readSource("src/common/app_server.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!protocol.empty(), "cell_protocol.h not found");
  TEST_ASSERT_TRUE_MESSAGE(!nodeHeader.empty(), "mesh_node.h not found");
  TEST_ASSERT_TRUE_MESSAGE(!nodeSource.empty(), "mesh_node.cpp not found");
  TEST_ASSERT_TRUE_MESSAGE(!server.empty(), "app_server.cpp not found");

  // No new bus frame type was introduced for calls, whatever their kind.
  TEST_ASSERT_FALSE(contains(server, "kTypeCall"));
  TEST_ASSERT_FALSE(contains(server, "kTypeAudio"));
  TEST_ASSERT_FALSE(contains(server, "kTypeLink"));
  TEST_ASSERT_FALSE(contains(protocol, "kTypeCall"));
  TEST_ASSERT_FALSE(contains(protocol, "kTypeAudio"));
  TEST_ASSERT_FALSE(contains(protocol, "kTypeLink"));

  // The mesh knows nothing about the calls or the backbone.
  const char* needles[] = {"CallTable", "kCallAudio", "handleCall", "call_audio", "cell_call.h",
                           "cell_link.h", "LinkCall", "StationLink", "WifiBackbone"};
  for (const char* needle : needles) {
    TEST_ASSERT_FALSE_MESSAGE(contains(nodeHeader, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(nodeSource, needle), needle);
    TEST_ASSERT_FALSE_MESSAGE(contains(protocol, needle), needle);
  }
}

void testTheBackboneFrameMagicCannotBeMistakenForABusFrame() {
  const uint8_t types[] = {
      cell::kTypePing,          cell::kTypePong,         cell::kTypeToken,
      cell::kTypeRouteState,    cell::kTypeRouteRequest, cell::kTypeBusAck,
      cell::kTypeText,          cell::kTypeReceipt,      cell::kTypeVoiceBlock,
      cell::kTypeVoiceFeedback, cell::kTypeAppControl,
  };
  for (size_t i = 0; i < sizeof(types); ++i) {
    TEST_ASSERT_TRUE(cell::kLinkMagic != types[i]);
    // The link message types must not collide with the bus message types either.
    TEST_ASSERT_TRUE(cell::kLinkHello != types[i]);
    TEST_ASSERT_TRUE(cell::kLinkCallSignal != types[i]);
    TEST_ASSERT_TRUE(cell::kLinkCallAudio != types[i]);
  }
  TEST_ASSERT_TRUE(cell::kLinkMagic != cell::kProtocolVersion);
  TEST_ASSERT_TRUE(cell::kLinkMagic != 0x00U);
  // A link frame carrying call audio is larger than any possible bus frame: it
  // could not even be encapsulated by accident.
  TEST_ASSERT_TRUE(cell::kLinkMaxFrameSize > cell::kMaxRawFrameSize);
}

void testTheRecordedVoicePathIsUntouchedByTheBackbone() {
  // The recorded voice notes keep their own binary frame, their own bus type and
  // their own magic: the backbone neither reuses nor replaces them.
  const std::string server = readSource("src/common/app_server.cpp");
  TEST_ASSERT_TRUE_MESSAGE(!server.empty(), "app_server.cpp not found");
  TEST_ASSERT_TRUE(contains(server, "kVoiceBinaryMagic"));
  TEST_ASSERT_TRUE(contains(server, "kTypeVoiceBlock"));
  TEST_ASSERT_TRUE(contains(server, "handleVoiceBlock"));
  // A live call frame (0xA7) is distinguished from a recorded voice block (0xA5)
  // by the first byte of the binary WebSocket payload.
  TEST_ASSERT_TRUE(contains(server, "payload[0] == kVoiceBinaryMagic"));
}
