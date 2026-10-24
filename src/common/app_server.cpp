#include "app_server.h"

#include <ArduinoJson.h>
#include <WiFi.h>

#include "log.h"

namespace cell {
namespace {

constexpr size_t kMaxVoiceChunk = 400;
constexpr size_t kVoiceHeaderSize = 22;  // msgId(16) + seq(2) + total(2) + crc(2)
// Binary voice frame sent on the WebSocket right after the JSON header:
//   [0xA5][0x01][msgId:16][seq:2 LE][total:2 LE][audio bytes...]
// The identity is repeated inside the binary frame on purpose: the JSON header
// and the binary frame travel on the same ordered socket, but a lost header used
// to make the application guess (and mix two voice notes). With the magic byte
// the receiver can always decide on its own which block it just received.
constexpr uint8_t kVoiceBinaryMagic = 0xA5U;
constexpr uint8_t kVoiceBinaryVersion = 0x01U;
constexpr size_t kVoiceBinaryOverhead = 22;  // magic + version + msgId + seq + total

const char* siteNameForNode(uint8_t node) {
  switch (node) {
    case 1: return "SITE_A";
    case 2: return "SITE_B";
    case 3: return "SITE_C";
    default: return "UNKNOWN";
  }
}

int base64Value(char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

size_t base64Decode(const char* input, size_t length, uint8_t* output, size_t capacity) {
  size_t written = 0;
  uint32_t accumulator = 0;
  uint8_t bits = 0;
  for (size_t i = 0; i < length; ++i) {
    if (input[i] == '=') {
      break;
    }
    const int value = base64Value(input[i]);
    if (value < 0) {
      continue;  // ignore whitespace and line breaks
    }
    accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
    bits = static_cast<uint8_t>(bits + 6U);
    if (bits >= 8U) {
      bits = static_cast<uint8_t>(bits - 8U);
      if (written >= capacity) {
        return 0;
      }
      output[written++] = static_cast<uint8_t>((accumulator >> bits) & 0xFFU);
    }
  }
  return written;
}

}  // namespace

AppServer::AppServer(MeshNode& mesh, StationLink& link, const StationConfig& config)
    : mesh_(mesh), link_(link), config_(config), http_(config.servicePort), ws_(config.wsPort) {
  memset(socketUsers_, 0, sizeof(socketUsers_));
}

void AppServer::begin() {
  http_.enableCORS(true);
  http_.on("/api/info", HTTP_GET, [this]() { handleInfo(); });
  http_.on("/api/health", HTTP_GET, [this]() { handleHealth(); });
  http_.on("/api/users", HTTP_GET, [this]() { handleUsers(); });
  http_.on("/api/presence", HTTP_POST, [this]() { handlePresenceRegister(); });
  http_.on("/api/presence/leave", HTTP_POST, [this]() { handlePresenceLeave(); });
  http_.on("/api/messages/text", HTTP_POST, [this]() { handleText(); });
  http_.on("/api/messages/receipt", HTTP_POST, [this]() { handleReceipt(); });
  http_.on("/api/voice/block", HTTP_POST, [this]() { handleVoiceBlock(); });
  http_.on("/api/voice/feedback", HTTP_POST, [this]() { handleVoiceFeedback(); });
  // Real time calls. New routes only: the existing ones are untouched.
  http_.on("/api/call/invite", HTTP_POST, [this]() { handleCallInvite(); });
  http_.on("/api/call/accept", HTTP_POST, [this]() { handleCallAccept(); });
  http_.on("/api/call/reject", HTTP_POST, [this]() { handleCallReject(); });
  http_.on("/api/call/cancel", HTTP_POST, [this]() { handleCallCancel(); });
  http_.on("/api/call/hangup", HTTP_POST, [this]() { handleCallHangup(); });
  http_.on("/api/call/state", HTTP_GET, [this]() { handleCallState(); });
  http_.onNotFound([this]() { handleNotFound(); });
  http_.begin();

  // The call table only trusts this station's own WebSocket bindings to decide
  // that a participant is local, and knows its own node id to decide whether a
  // session goes over the Wi-Fi backbone.
  calls_.begin(&AppServer::probeLocalUser, this, config_.nodeId);

  ws_.begin();
  ws_.onEvent([this](uint8_t clientNumber, WStype_t type, uint8_t* payload, size_t length) {
    handleSocketEvent(clientNumber, static_cast<uint8_t>(type), payload, length);
  });
  ws_.enableHeartbeat(15000, 3000, 2);

  LOG_I("http", "REST http://%s:%u  WebSocket ws://%s:%u", config_.apIp,
        static_cast<unsigned>(config_.servicePort), config_.apIp,
        static_cast<unsigned>(config_.wsPort));
}

void AppServer::loop() {
  http_.handleClient();
  ws_.loop();
  // Expires unanswered invitations and calls that stopped carrying media. Bounded
  // work (one session per call), so the main loop is never blocked.
  serviceCallTimeouts();
}

void AppServer::sendJson(int code, const String& body) {
  http_.send(code, "application/json", body);
}

void AppServer::sendStatus(int code, const char* status, const char* detail) {
  JsonDocument doc;
  doc["status"] = status;
  if (detail != nullptr) {
    doc["detail"] = detail;
  }
  String out;
  serializeJson(doc, out);
  sendJson(code, out);
}

namespace {

void appendJsonEscaped(String& out, const char* text) {
  if (text == nullptr) {
    return;
  }
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    const char value = *cursor;
    if (value == '"' || value == '\\') {
      out += '\\';
      out += value;
    } else if (static_cast<unsigned char>(value) < 0x20U) {
      out += ' ';
    } else {
      out += value;
    }
  }
}

// Copies a bounded, zero padded identifier into a fixed width field.
void copyField(char* destination, size_t capacity, const char* source) {
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

}  // namespace

void AppServer::handleSocketEvent(uint8_t clientNumber,
                                  uint8_t type,
                                  uint8_t* payload,
                                  size_t length) {
  if (clientNumber >= WEBSOCKETS_SERVER_CLIENT_MAX) {
    return;
  }
  ++socketEvents_;
  switch (static_cast<WStype_t>(type)) {
    case WStype_DISCONNECTED: {
      // A user that leaves the cell can no longer be part of a call: the session
      // is closed and the other party is notified, so no resource leaks.
      char leaving[kMaxUserIdLength] = {0};
      strncpy(leaving, socketUsers_[clientNumber], kMaxUserIdLength - 1U);
      socketUsers_[clientNumber][0] = '\0';
      if (leaving[0] != '\0') {
        endCallsForUser(leaving, "call_ended", "disconnected");
      }
      LOG_I("ws", "socket %u disconnected", static_cast<unsigned>(clientNumber));
      break;
    }

    case WStype_CONNECTED:
      LOG_I("ws", "socket %u connected, waiting for hello", static_cast<unsigned>(clientNumber));
      break;

    case WStype_BIN:
      // Binary payloads are either a recorded voice block (0xA5) or a live call
      // media frame (0xA7). The dispatch is explicit so the two features can
      // never be confused.
      if (length > 0U && payload[0] == kVoiceBinaryMagic) {
        // Recorded voice notes are pushed by the station to the application;
        // a client never sends one back, so nothing to do here.
        LOG_D("ws", "binary voice frame from socket %u ignored",
              static_cast<unsigned>(clientNumber));
        break;
      }
      handleCallAudio(clientNumber, payload, length);
      break;

    case WStype_TEXT: {
      JsonDocument doc;
      const DeserializationError error =
          deserializeJson(doc, reinterpret_cast<const char*>(payload), length);
      if (error) {
        LOG_W("ws", "invalid client message: %s", error.c_str());
        return;
      }
      const char* messageType = doc["type"] | "";
      if (strcmp(messageType, "hello") != 0) {
        return;
      }
      const char* userId = doc["userId"] | "";
      const char* displayName = doc["displayName"] | "";
      bindSocketToUser(clientNumber, userId, displayName);
      if (socketUsers_[clientNumber][0] == '\0') {
        return;
      }

      String users;
      buildUserListJson(users);
      String answer;
      answer.reserve(256 + users.length());
      answer += "{\"type\":\"hello_ack\",\"cell\":\"";
      answer += config_.siteId;
      answer += "\",\"ssid\":\"";
      answer += config_.wifiSsid;
      answer += "\",\"node\":";
      answer += String(config_.nodeId);
      answer += ",\"wsPort\":";
      answer += String(config_.wsPort);
      answer += ",\"users\":";
      answer += users;
      answer += "}";
      ws_.sendTXT(clientNumber, answer.c_str());
      break;
    }

    default:
      break;
  }
}

void AppServer::bindSocketToUser(uint8_t clientNumber,
                                 const char* userId,
                                 const char* displayName) {
  if (userId == nullptr || userId[0] == '\0' || strlen(userId) >= kMaxUserIdLength) {
    socketUsers_[clientNumber][0] = '\0';
    LOG_W("ws", "invalid user id on socket %u", static_cast<unsigned>(clientNumber));
    return;
  }
  memset(socketUsers_[clientNumber], 0, kMaxUserIdLength);
  strncpy(socketUsers_[clientNumber], userId, kMaxUserIdLength - 1U);
  // The identity of a user is its identifier, never its name, its address or the
  // cell it is attached to. Joining a new cell therefore only refreshes the
  // presence entry: no new user is ever created.
  //
  // The display name comes from the client. Without it, a user that moves to
  // another cell would be registered under its raw identifier and every cell
  // would then display "u-3f2a1b9c0d" instead of the announced name. The
  // identifier is only kept as a last resort for an older client.
  const char* name =
      (displayName != nullptr && displayName[0] != '\0') ? displayName : socketUsers_[clientNumber];
  mesh_.refreshLocalUser(socketUsers_[clientNumber]);
  if (!mesh_.registerLocalUser(socketUsers_[clientNumber], name)) {
    LOG_W("ws", "cannot register user %s on this station (presence table full?)",
          socketUsers_[clientNumber]);
  }
  LOG_I("ws", "socket %u bound to user %s (%s)", static_cast<unsigned>(clientNumber),
        socketUsers_[clientNumber], name);
}

void AppServer::pushToUser(const char* userId, const String& json) {
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (socketUsers_[i][0] != '\0' && strcmp(socketUsers_[i], userId) == 0) {
      ws_.sendTXT(i, json.c_str());
      ++pushedEvents_;
    }
  }
}

void AppServer::pushVoiceBlockToUser(const char* userId,
                                     const char* msgId,
                                     uint16_t seq,
                                     uint16_t total,
                                     const uint8_t* data,
                                     size_t length) {
  if (msgId == nullptr || strlen(msgId) < 16U || data == nullptr ||
      length > kMaxVoiceChunk) {
    LOG_W("ws", "voice block refused (msgId or payload invalid)");
    return;
  }
  uint8_t envelope[kVoiceBinaryOverhead + kMaxVoiceChunk];
  envelope[0] = kVoiceBinaryMagic;
  envelope[1] = kVoiceBinaryVersion;
  memcpy(envelope + 2, msgId, 16);
  envelope[18] = static_cast<uint8_t>(seq & 0xFFU);
  envelope[19] = static_cast<uint8_t>(seq >> 8);
  envelope[20] = static_cast<uint8_t>(total & 0xFFU);
  envelope[21] = static_cast<uint8_t>(total >> 8);
  memcpy(envelope + kVoiceBinaryOverhead, data, length);

  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (socketUsers_[i][0] != '\0' && strcmp(socketUsers_[i], userId) == 0) {
      ws_.sendBIN(i, envelope, kVoiceBinaryOverhead + length);
      ++pushedEvents_;
    }
  }
}

size_t AppServer::buildUserListJson(String& out) const {
  out = "[";
  const RoutingTable& table = mesh_.routing();
  bool first = true;
  for (size_t i = 0; i < table.entries(); ++i) {
    const RouteEntry& entry = table.at(i);
    if (!first) {
      out += ',';
    }
    first = false;
    out += "{\"id\":\"";
    appendJsonEscaped(out, entry.userId);
    out += "\",\"name\":\"";
    appendJsonEscaped(out, entry.displayName);
    out += "\",\"station\":\"";
    out += siteNameForNode(entry.station);
    out += "\",\"stationNode\":";
    out += String(entry.station);
    out += ",\"online\":";
    out += entry.online ? "true" : "false";
    out += '}';
  }
  out += ']';
  return out.length();
}

void AppServer::pushPresence() {
  String users;
  buildUserListJson(users);
  String event;
  event.reserve(users.length() + 64U);
  event += "{\"type\":\"presence\",\"cell\":\"";
  event += config_.siteId;
  event += "\",\"users\":";
  event += users;
  event += "}";
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (socketUsers_[i][0] != '\0') {
      ws_.sendTXT(i, event.c_str());
      ++pushedEvents_;
    }
  }
}

void AppServer::onPresenceChanged() {
  pushPresence();
}

void AppServer::onAppFrame(uint8_t type, const uint8_t* payload, size_t length) {
  AppEnvelope envelope;
  if (!parseAppEnvelope(payload, length, envelope)) {
    LOG_W("app", "cannot parse envelope (%u bytes)", static_cast<unsigned>(length));
    return;
  }

  switch (type) {
    case kTypeText:
    case kTypeReceipt:
    case kTypeAppControl:
    case kTypeVoiceFeedback: {
      String json;
      json.reserve(envelope.bodyLength + 1U);
      json.concat(reinterpret_cast<const char*>(envelope.body),
                  static_cast<unsigned int>(envelope.bodyLength));
      pushToUser(envelope.destination, json);
      break;
    }

    case kTypeVoiceBlock: {
      if (envelope.bodyLength <= kVoiceHeaderSize) {
        LOG_W("app", "voice block too short");
        return;
      }
      char msgId[17] = {0};
      memcpy(msgId, envelope.body, 16);
      const uint16_t seq = static_cast<uint16_t>(envelope.body[16]) |
                           (static_cast<uint16_t>(envelope.body[17]) << 8);
      const uint16_t total = static_cast<uint16_t>(envelope.body[18]) |
                             (static_cast<uint16_t>(envelope.body[19]) << 8);
      const uint16_t crc = static_cast<uint16_t>(envelope.body[20]) |
                           (static_cast<uint16_t>(envelope.body[21]) << 8);
      const size_t dataLength = envelope.bodyLength - kVoiceHeaderSize;

      JsonDocument doc;
      doc["type"] = "voice_block";
      doc["msgId"] = msgId;
      doc["from"] = envelope.source;
      doc["to"] = envelope.destination;
      doc["seq"] = seq;
      doc["total"] = total;
      doc["crc"] = crc;
      doc["length"] = dataLength;

      String header;
      serializeJson(doc, header);
      pushToUser(envelope.destination, header);
      // The binary frame repeats msgId/seq/total: the receiving application never
      // has to remember the header to identify the block it is handling.
      pushVoiceBlockToUser(envelope.destination, msgId, seq, total,
                           envelope.body + kVoiceHeaderSize, dataLength);
      LOG_D("ws", "voice block %s %u/%u forwarded to %s (%u bytes)", msgId,
            static_cast<unsigned>(seq + 1U), static_cast<unsigned>(total),
            envelope.destination, static_cast<unsigned>(dataLength));
      break;
    }

    default:
      LOG_D("app", "unhandled frame type %s", messageTypeName(type));
      break;
  }
}

void AppServer::handleInfo() {
  JsonDocument doc;
  doc["site"] = config_.siteId;
  doc["cell"] = config_.siteName;
  doc["ssid"] = config_.wifiSsid;
  doc["node"] = config_.nodeId;
  doc["firmware"] = config_.firmware;
  doc["ip"] = WiFi.softAPIP().toString();
  doc["httpPort"] = config_.servicePort;
  doc["wsPort"] = config_.wsPort;
  doc["uptime"] = mesh_.uptimeSeconds();
  doc["terminated"] = config_.rs485Terminated;
  doc["users"] = mesh_.routing().entries();
  // Additive: real time calls are local to this cell, so only a local count is
  // meaningful here.
  doc["calls"] = calls_.activeSessions();

  JsonArray peers = doc["peers"].to<JsonArray>();
  for (uint8_t node = 1; node <= kMaxStationNodes; ++node) {
    JsonObject peer = peers.add<JsonObject>();
    peer["node"] = node;
    peer["site"] = siteNameForNode(node);
    peer["alive"] = mesh_.arbiter().isAlive(node);
  }

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void AppServer::handleHealth() {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["site"] = config_.siteId;
  doc["uptime"] = mesh_.uptimeSeconds();
  doc["heap"] = ESP.getFreeHeap();

  JsonObject bus = doc["bus"].to<JsonObject>();
  bus["sent"] = mesh_.bus().framesSent();
  bus["received"] = mesh_.bus().framesReceived();
  bus["txBytes"] = mesh_.bus().txBytes();
  bus["rxBytes"] = mesh_.bus().rxBytes();
  bus["crcErrors"] = mesh_.bus().decodeErrors();
  bus["cobsErrors"] = mesh_.bus().assemblerErrors();
  bus["cobsOverflows"] = mesh_.bus().assemblerOverflows();
  bus["txRejected"] = mesh_.bus().txRejected();
  bus["retries"] = mesh_.transportRetries();
  bus["failures"] = mesh_.transportFailures();
  bus["ackTimeouts"] = mesh_.ackTimeouts();
  bus["duplicates"] = mesh_.duplicatesDropped();
  bus["tokensReceived"] = mesh_.tokensReceived();
  bus["tokenPasses"] = mesh_.tokenPasses();
  bus["tokensClaimed"] = mesh_.tokensClaimed();
  bus["idleRounds"] = mesh_.arbiter().idleRounds();
  bus["aliveMask"] = mesh_.arbiter().aliveMask();
  bus["hasToken"] = mesh_.arbiter().hasToken();
  bus["queueSize"] = mesh_.queue().size();
  bus["queueControl"] = mesh_.queue().controlSize();
  bus["queueDrops"] = mesh_.queueDrops();
  bus["queueControlDrops"] = mesh_.controlQueueDrops();
  bus["queueControlCapacity"] = kControlQueueCapacity;
  bus["pendingAcks"] = mesh_.acks().pendingFragments();
  bus["fragmentsRejected"] = mesh_.fragmentsRejected();
  bus["reassembled"] = mesh_.reassembledMessages();
  bus["presenceExpirations"] = mesh_.presenceExpirations();
  bus["maxFragments"] = kMaxFragments;
  bus["maxPayload"] = kMaxReassembledPayload;

  JsonObject system = doc["system"].to<JsonObject>();
  system["heap"] = ESP.getFreeHeap();
  system["heapMin"] = ESP.getMinFreeHeap();
  system["heapMaxBlock"] = ESP.getMaxAllocHeap();
  system["maxLoopUs"] = mesh_.maxLoopMicros();

  JsonObject sockets = doc["sockets"].to<JsonObject>();
  sockets["events"] = socketEvents_;
  sockets["pushed"] = pushedEvents_;

  // Additive block: local real time calls. Existing consumers ignore it, and the
  // counters make it obvious that call audio is relayed locally and never
  // submitted to the bus (there is no bus counter here on purpose).
  JsonObject calls = doc["calls"].to<JsonObject>();
  calls["active"] = calls_.activeSessions();
  calls["completed"] = calls_.completedCalls();
  calls["refused"] = calls_.refusedCalls();
  calls["lastRefusal"] = calls_.lastRefusal();
  calls["audioFrames"] = callAudioFrames_;
  calls["audioRelayed"] = callAudioRelayed_;
  calls["audioRejected"] = callAudioRejected_;
  calls["audioSilent"] = callAudioSilent_;
  calls["transport"] = "websocket-binary";
  calls["sampleRate"] = 8000;
  calls["frameMs"] = 40;
  calls["remoteCalls"] = callRemote_;
  calls["audioForwarded"] = callAudioForwarded_;
  calls["audioFromLink"] = callAudioFromLink_;
  calls["signalsSent"] = callSignalsSent_;
  calls["signalsReceived"] = callSignalsReceived_;

  // Additive block: state of the inter-station Wi-Fi backbone. A cross cell call
  // uses this link for its signalling and for its audio; the bus counters below
  // are untouched by a call, whatever its kind.
  JsonObject link = doc["link"].to<JsonObject>();
  link["node"] = config_.nodeId;
  link["port"] = config_.linkPort;
  link["upstream"] = link_.routes().upstream();
  link["upstreamReady"] = link_.upstreamReady();
  link["upstreamConnected"] = link_.upstream().connected();
  link["peers"] = link_.directMask();
  link["framesSent"] = link_.framesSent();
  link["framesReceived"] = link_.framesReceived();
  link["framesRelayed"] = link_.framesRelayed();
  link["framesDropped"] = link_.framesDropped();
  link["signalsSent"] = link_.signalsSent();
  link["signalsReceived"] = link_.signalsReceived();
  link["audioSent"] = link_.audioSent();
  link["audioReceived"] = link_.audioReceived();
  link["authRejections"] = link_.authRejections();
  link["disconnects"] = link_.disconnects();
  // Diagnosis of the two things that can starve the RS485 bus: how much the
  // backbone still has to write, and how long the link servicing blocked the
  // main loop. Both must stay small; a growing txPending means a congested
  // backbone, a large maxLoopUs means the bus receive buffer was at risk.
  link["txPending"] = static_cast<uint32_t>(link_.txPending());
  link["maxLoopUs"] = link_.maxLoopUs();
  link["transport"] = "tcp-wifi-backbone";

  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

void AppServer::handleUsers() {
  String users;
  buildUserListJson(users);
  String out;
  out.reserve(users.length() + 96U);
  out += "{\"cell\":\"";
  out += config_.siteId;
  out += "\",\"node\":";
  out += String(config_.nodeId);
  out += ",\"users\":";
  out += users;
  out += '}';
  sendJson(200, out);
}

void AppServer::handlePresenceRegister() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* userId = doc["userId"] | "";
  const char* displayName = doc["displayName"] | "";
  if (userId[0] == '\0') {
    sendStatus(400, "missing-user-id");
    return;
  }
  if (!mesh_.registerLocalUser(userId, displayName[0] == '\0' ? userId : displayName)) {
    sendStatus(503, "presence-table-full");
    return;
  }
  mesh_.publishPresence();

  String users;
  buildUserListJson(users);
  String out;
  out.reserve(users.length() + 96U);
  out += "{\"status\":\"registered\",\"cell\":\"";
  out += config_.siteId;
  out += "\",\"node\":";
  out += String(config_.nodeId);
  out += ",\"users\":";
  out += users;
  out += '}';
  sendJson(200, out);
  pushPresence();
}

void AppServer::handlePresenceLeave() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* userId = doc["userId"] | "";
  if (userId[0] == '\0') {
    sendStatus(400, "missing-user-id");
    return;
  }
  mesh_.removeLocalUser(userId);
  sendStatus(200, "left");
  pushPresence();
}

// ---------------------------------------------------------------------------
// Real time calls - signalling
//
// Everything below is local to this cell. None of these handlers calls
// mesh_.submitAppPayload(): a call never enters the RS485 egress queue and is
// never routed to another station. The media itself is relayed by
// handleCallAudio() between two WebSocket clients of this same station.
// ---------------------------------------------------------------------------
void AppServer::handleCallInvite() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* callId = doc["callId"] | "";
  const char* from = doc["from"] | "";
  const char* to = doc["to"] | "";
  if (callId[0] == '\0' || from[0] == '\0' || to[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }

  // The recipient must be known and online. The routing table is the presence
  // information maintained by the RS485 mesh: it is reused as is, never
  // duplicated, and only tells us *where* the user is.
  const RouteEntry* target = mesh_.routing().find(to);
  if (target == nullptr) {
    sendStatus(404, "unknown-user", "the recipient was never seen on this network");
    return;
  }
  if (!target->online) {
    sendStatus(409, "recipient-offline", "the recipient is not connected to this network");
    return;
  }

  if (target->station != config_.nodeId) {
    // The recipient is in another cell: the signalling goes over the Wi-Fi
    // backbone. The audio will follow the same path once the call is accepted.
    handleCallInviteRemote(callId, from, to, *target);
    return;
  }

  // Local call: both participants must hold a live socket on *this* station.
  // The declared "cell" of a client is never trusted. Unchanged path.
  const CallStatus status = calls_.invite(callId, from, to, millis());
  if (status != CallStatus::kOk) {
    replyCallStatus(status);
    return;
  }
  const CallSession* session = calls_.find(callId);
  if (session == nullptr) {
    sendStatus(500, "internal-error");
    return;
  }
  pushCallEvent(session->callee, "call_invite", *session, nullptr, nullptr);
  pushCallEvent(session->caller, "call_ringing", *session, nullptr, nullptr);
  LOG_I("call", "%s invites %s (%s) on %s", session->caller, session->callee, session->callId,
        config_.siteId);

  JsonDocument out;
  out["status"] = "ringing";
  out["callId"] = session->callId;
  out["cell"] = config_.siteId;
  out["to"] = session->callee;
  out["remote"] = false;
  String text;
  serializeJson(out, text);
  sendJson(202, text);
}

void AppServer::handleCallAccept() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* callId = doc["callId"] | "";
  const char* userId = doc["userId"] | "";
  if (callId[0] == '\0' || userId[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  const CallStatus status = calls_.accept(callId, userId, millis());
  if (status != CallStatus::kOk) {
    replyCallStatus(status);
    return;
  }
  const CallSession* session = calls_.find(callId);
  if (session == nullptr) {
    sendStatus(500, "internal-error");
    return;
  }
  if (session->remoteNode == 0U) {
    // Local call: both participants open the media channel at the same instant.
    pushCallEvent(session->caller, "call_accepted", *session, nullptr, nullptr);
    pushCallEvent(session->callee, "call_accepted", *session, nullptr, nullptr);
  } else {
    // One participant is in another cell: it is notified over the backbone, and
    // only the local participant receives the WebSocket event.
    sendRemoteSignal(*session, kSignalAccept);
    const char* local = localParticipant(*session);
    if (local != nullptr) {
      pushCallEvent(local, "call_accepted", *session, nullptr, nullptr);
    }
  }
  LOG_I("call", "%s accepted the call of %s (%s)%s", session->callee, session->caller,
        session->callId, session->remoteNode == 0U ? "" : " across the Wi-Fi backbone");
  sendStatus(200, "active");
}

void AppServer::handleCallReject() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* callId = doc["callId"] | "";
  const char* userId = doc["userId"] | "";
  if (callId[0] == '\0' || userId[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  const CallSession* session = calls_.find(callId);
  char caller[kCallUserLength] = {0};
  uint8_t remoteNode = 0;
  if (session != nullptr) {
    strncpy(caller, session->caller, sizeof(caller) - 1U);
    remoteNode = session->remoteNode;
  }
  const CallStatus status = calls_.reject(callId, userId, millis());
  if (status != CallStatus::kOk) {
    replyCallStatus(status);
    return;
  }
  notifyCallEnded(caller, callId, userId, "call_rejected", "rejected");
  if (remoteNode != 0U) {
    // The caller is in another cell: it must learn that the call was refused.
    LinkCallSignal signal;
    copyField(signal.callId, sizeof(signal.callId), callId);
    signal.action = kSignalReject;
    signal.reason = static_cast<uint8_t>(CallEndReason::kRejected);
    copyField(signal.from, sizeof(signal.from), caller);
    copyField(signal.to, sizeof(signal.to), userId);
    if (link_.sendSignal(signal, remoteNode)) {
      ++callSignalsSent_;
    }
  }
  LOG_I("call", "%s rejected the call of %s (%s)", userId, caller, callId);
  sendStatus(200, "rejected");
}

void AppServer::handleCallCancel() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* callId = doc["callId"] | "";
  const char* userId = doc["userId"] | "";
  if (callId[0] == '\0' || userId[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  const CallSession* session = calls_.find(callId);
  char callee[kCallUserLength] = {0};
  uint8_t remoteNode = 0;
  if (session != nullptr) {
    strncpy(callee, session->callee, sizeof(callee) - 1U);
    remoteNode = session->remoteNode;
  }
  const CallStatus status = calls_.cancel(callId, userId, millis());
  if (status != CallStatus::kOk) {
    replyCallStatus(status);
    return;
  }
  notifyCallEnded(callee, callId, userId, "call_cancelled", "cancelled");
  if (remoteNode != 0U) {
    // The recipient is in another cell: its station must stop ringing.
    LinkCallSignal signal;
    copyField(signal.callId, sizeof(signal.callId), callId);
    signal.action = kSignalCancel;
    signal.reason = static_cast<uint8_t>(CallEndReason::kCancelled);
    copyField(signal.from, sizeof(signal.from), userId);
    copyField(signal.to, sizeof(signal.to), callee);
    if (link_.sendSignal(signal, remoteNode)) {
      ++callSignalsSent_;
    }
  }
  LOG_I("call", "%s cancelled the call to %s (%s)", userId, callee, callId);
  sendStatus(200, "cancelled");
}

void AppServer::handleCallHangup() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* callId = doc["callId"] | "";
  const char* userId = doc["userId"] | "";
  if (callId[0] == '\0' || userId[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  const CallSession* session = calls_.find(callId);
  char peer[kCallUserLength] = {0};
  uint8_t remoteNode = 0;
  if (session != nullptr) {
    const char* other = strcmp(session->caller, userId) == 0 ? session->callee : session->caller;
    strncpy(peer, other, sizeof(peer) - 1U);
    remoteNode = session->remoteNode;
  }
  const CallStatus status = calls_.hangup(callId, userId, millis());
  if (status != CallStatus::kOk) {
    replyCallStatus(status);
    return;
  }
  notifyCallEnded(peer, callId, userId, "call_ended", "hangup");
  if (remoteNode != 0U) {
    // The other participant is in another cell: its station releases its half
    // and stops the media path immediately.
    LinkCallSignal signal;
    copyField(signal.callId, sizeof(signal.callId), callId);
    signal.action = kSignalHangup;
    signal.reason = static_cast<uint8_t>(CallEndReason::kHungUp);
    copyField(signal.from, sizeof(signal.from), userId);
    copyField(signal.to, sizeof(signal.to), peer);
    if (link_.sendSignal(signal, remoteNode)) {
      ++callSignalsSent_;
    }
  }
  LOG_I("call", "%s ended the call %s", userId, callId);
  sendStatus(200, "ended");
}

// ---------------------------------------------------------------------------
// Real time calls - cross cell over the Wi-Fi backbone
//
// The signalling and the audio of a cross cell call go over the inter-station
// IP link (lib/cell_link). No handler below ever hands anything to the mesh: a
// call never enters the RS485 egress queue and no bus frame type was added.
// ---------------------------------------------------------------------------

// Outgoing invitation whose recipient lives in another cell.
void AppServer::handleCallInviteRemote(const char* callId,
                                       const char* from,
                                       const char* to,
                                       const RouteEntry& target) {
  if (!link_.reachable(target.station)) {
    sendStatus(409, "cell-unreachable",
               "the station hosting the recipient is not reachable over the Wi-Fi backbone");
    return;
  }
  const CallStatus status = calls_.inviteRemote(callId, from, to, target.station, millis());
  if (status != CallStatus::kOk) {
    replyCallStatus(status);
    return;
  }
  const CallSession* session = calls_.find(callId);
  if (session == nullptr) {
    sendStatus(500, "internal-error");
    return;
  }
  if (!sendRemoteSignal(*session, kSignalInvite)) {
    CallSession dropped;
    calls_.remoteEnd(callId, millis(), dropped);
    sendStatus(503, "link-unavailable", "the Wi-Fi backbone could not carry the invitation");
    return;
  }
  ++callRemote_;
  // The caller is told "dialing": the call is reported as ringing only once the
  // remote station confirms that the recipient's phone really rings.
  pushCallEvent(session->caller, "call_dialing", *session, nullptr, nullptr);
  LOG_I("call", "%s invites %s (%s) in cell node %u over the Wi-Fi backbone", session->caller,
        session->callee, session->callId, static_cast<unsigned>(session->remoteNode));

  JsonDocument out;
  out["status"] = "ringing";
  out["callId"] = session->callId;
  out["cell"] = config_.siteId;
  out["to"] = session->callee;
  out["remote"] = true;
  out["route"] = siteNameForNode(target.station);
  String text;
  serializeJson(out, text);
  sendJson(202, text);
}

bool AppServer::sendRemoteSignal(const CallSession& session, uint8_t action, uint8_t reason) {
  if (session.remoteNode == 0U) {
    return false;
  }
  LinkCallSignal signal;
  copyField(signal.callId, sizeof(signal.callId), session.callId);
  signal.action = action;
  signal.reason = reason;
  copyField(signal.from, sizeof(signal.from), session.caller);
  copyField(signal.to, sizeof(signal.to), session.callee);
  const bool sent = link_.sendSignal(signal, session.remoteNode);
  if (sent) {
    ++callSignalsSent_;
  }
  return sent;
}

const char* AppServer::localParticipant(const CallSession& session) const {
  if (hasLocalSocket(session.caller)) {
    return session.caller;
  }
  if (hasLocalSocket(session.callee)) {
    return session.callee;
  }
  return nullptr;
}

// Ends a cross cell session and tells the other station, so both halves are
// released at the same moment.
void AppServer::releaseRemoteSession(const CallSession& session, uint8_t action, uint8_t reason) {
  if (session.remoteNode == 0U) {
    return;
  }
  sendRemoteSignal(session, action, reason);
}

// Diagnostic view of the call state of one user on this cell. Read only.
void AppServer::handleCallState() {
  const char* userId = http_.arg("userId").c_str();
  const CallSession* session =
      (userId != nullptr && userId[0] != '\0') ? calls_.sessionOf(userId) : nullptr;
  JsonDocument doc;
  doc["cell"] = config_.siteId;
  doc["node"] = config_.nodeId;
  doc["active"] = calls_.activeSessions();
  doc["completed"] = calls_.completedCalls();
  doc["refused"] = calls_.refusedCalls();
  doc["lastRefusal"] = calls_.lastRefusal();
  doc["audioFrames"] = callAudioFrames_;
  doc["audioRelayed"] = callAudioRelayed_;
  doc["audioRejected"] = callAudioRejected_;
  doc["audioSilent"] = callAudioSilent_;
  JsonObject media = doc["audio"].to<JsonObject>();
  media["sampleRate"] = 8000;
  media["channels"] = 1;
  media["bitsPerSample"] = 16;
  media["frameMs"] = 40;
  media["frameBytes"] = kCallAudioFrameBytes;
  media["headerBytes"] = kCallAudioHeaderSize;
  media["transport"] = "websocket-binary";
  if (session != nullptr) {
    JsonObject call = doc["call"].to<JsonObject>();
    call["callId"] = session->callId;
    call["state"] = callStateName(session->state);
    call["from"] = session->caller;
    call["to"] = session->callee;
    call["role"] = strcmp(session->caller, userId) == 0 ? "caller" : "callee";
    call["remote"] = session->remoteNode != 0U;
    call["remoteNode"] = session->remoteNode;
    call["remoteCell"] = session->remoteNode == 0U ? config_.siteId
                                                   : siteNameForNode(session->remoteNode);
    call["remoteConfirmed"] = session->remoteConfirmed;
  }
  // Additive: the state of the Wi-Fi backbone as seen from this station.
  JsonObject link = doc["link"].to<JsonObject>();
  link["upstream"] = link_.routes().upstream();
  link["upstreamReady"] = link_.upstreamReady();
  link["upstreamConnected"] = link_.upstream().connected();
  link["peers"] = link_.directMask();
  JsonArray reachable = link["reachable"].to<JsonArray>();
  for (uint8_t node = 1; node <= kCallMaxNodes; ++node) {
    if (link_.reachable(node)) {
      reachable.add(node);
    }
  }
  link["signalsSent"] = callSignalsSent_;
  link["signalsReceived"] = callSignalsReceived_;
  link["audioForwarded"] = callAudioForwarded_;
  link["audioFromLink"] = callAudioFromLink_;
  link["remoteCalls"] = callRemote_;
  link["txPending"] = static_cast<uint32_t>(link_.txPending());
  link["maxLoopUs"] = link_.maxLoopUs();
  String out;
  serializeJson(doc, out);
  sendJson(200, out);
}

// ---------------------------------------------------------------------------
// Real time calls - media relay
//
// This is the whole audio path of a call: a binary WebSocket frame received
// from one client of this station is forwarded to the peer client of the same
// station. No RS485 frame, no mesh queue, no HTTP body, no base64.
// ---------------------------------------------------------------------------
void AppServer::handleCallAudio(uint8_t clientNumber, uint8_t* payload, size_t length) {
  ++callAudioFrames_;
  if (clientNumber >= WEBSOCKETS_SERVER_CLIENT_MAX) {
    ++callAudioRejected_;
    return;
  }
  const char* sender = socketUsers_[clientNumber];
  if (sender[0] == '\0') {
    // A socket that did not announce itself cannot carry media.
    ++callAudioRejected_;
    return;
  }
  char callId[kCallIdLength + 1] = {0};
  uint16_t sequence = 0;
  const uint8_t* media = nullptr;
  size_t mediaLength = 0;
  if (!parseCallAudioFrame(payload, length, callId, sizeof(callId), sequence, &media,
                           mediaLength)) {
    ++callAudioRejected_;
    LOG_D("call", "malformed media frame from socket %u (%u bytes)",
          static_cast<unsigned>(clientNumber), static_cast<unsigned>(length));
    return;
  }
  // Only an established session of this station can relay, and only between its
  // two participants.
  const char* peer = calls_.mediaPeer(callId, sender);
  if (peer == nullptr) {
    ++callAudioRejected_;
    return;
  }

  const uint8_t remoteNode = calls_.remoteNodeOf(callId);
  if (remoteNode != 0U) {
    // Cross cell call: the frame leaves this cell over the Wi-Fi backbone. It is
    // never handed to the RS485 egress queue and no bus frame is created.
    if (link_.sendAudio(payload, length, remoteNode)) {
      calls_.noteMedia(callId, millis());
      ++callAudioForwarded_;
    } else {
      // The backbone is down: the media watchdog closes the session shortly, and
      // the audio is NOT rerouted on the bus.
      ++callAudioSilent_;
    }
    return;
  }

  size_t delivered = 0;
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (i == clientNumber || socketUsers_[i][0] == '\0') {
      continue;
    }
    if (strcmp(socketUsers_[i], peer) != 0) {
      continue;
    }
    if (ws_.sendBIN(i, payload, length)) {
      ++delivered;
    }
  }
  if (delivered == 0U) {
    // The peer socket vanished: the media watchdog closes the session shortly.
    ++callAudioSilent_;
    return;
  }
  calls_.noteMedia(callId, millis());
  callAudioRelayed_ += static_cast<uint32_t>(delivered);
}

// ---------------------------------------------------------------------------
// Real time calls - reception from another cell
//
// The other station of a cross cell call hands us its signalling and its audio
// here. Both stay inside this station from that point: the audio is delivered to
// the local participant's WebSocket, exactly like a local call.
// ---------------------------------------------------------------------------
void AppServer::onLinkSignal(const LinkCallSignal& signal, uint8_t fromNode) {
  ++callSignalsReceived_;
  const uint32_t now = millis();
  switch (signal.action) {
    case kSignalInvite: {
      // The callee is here, the caller is on `fromNode`.
      const CallStatus status =
          calls_.inviteIncoming(signal.callId, signal.from, signal.to, fromNode, now);
      if (status != CallStatus::kOk) {
        // The caller must not stay in "dialing" for ever: it is told why.
        LinkCallSignal answer;
        copyField(answer.callId, sizeof(answer.callId), signal.callId);
        copyField(answer.from, sizeof(answer.from), signal.from);
        copyField(answer.to, sizeof(answer.to), signal.to);
        if (status == CallStatus::kBusy) {
          answer.action = kSignalEnded;
          answer.reason = static_cast<uint8_t>(CallEndReason::kBusy);
        } else {
          answer.action = kSignalReject;
          answer.reason = static_cast<uint8_t>(CallEndReason::kUnavailable);
        }
        if (link_.sendSignal(answer, fromNode)) {
          ++callSignalsSent_;
        }
        LOG_W("call", "cross cell invitation %s refused: %s", signal.callId,
              callStatusName(status));
        return;
      }
      const CallSession* session = calls_.find(signal.callId);
      if (session == nullptr) {
        return;
      }
      ++callRemote_;
      // The recipient's phone rings here; the caller's station is confirmed.
      pushCallEvent(session->callee, "call_invite", *session, nullptr, nullptr);
      if (sendRemoteSignal(*session, kSignalRinging)) {
        LOG_I("call", "incoming cross cell call %s from %s (node %u)", signal.callId, signal.from,
              static_cast<unsigned>(fromNode));
      }
      return;
    }

    case kSignalRinging: {
      if (calls_.noteRemoteRinging(signal.callId, now) != CallStatus::kOk) {
        return;
      }
      const CallSession* session = calls_.find(signal.callId);
      if (session == nullptr) {
        return;
      }
      pushCallEvent(session->caller, "call_ringing", *session, nullptr, nullptr);
      return;
    }

    case kSignalAccept: {
      if (calls_.acceptRemote(signal.callId, now) != CallStatus::kOk) {
        return;
      }
      const CallSession* session = calls_.find(signal.callId);
      if (session == nullptr) {
        return;
      }
      // The media path really exists now: the caller's application may open its
      // microphone. A call is never reported as connected before this point.
      pushCallEvent(session->caller, "call_accepted", *session, nullptr, nullptr);
      LOG_I("call", "cross cell call %s established with node %u", signal.callId,
            static_cast<unsigned>(fromNode));
      return;
    }

    case kSignalReject:
    case kSignalCancel:
    case kSignalHangup:
    case kSignalEnded: {
      // `remoteEnd` returns a copy of the released session: the local
      // participant must be notified AFTER the release, and the session fields
      // are cleared by then. Reading them through a pointer into the table would
      // notify nobody at all, which is exactly why the remote side kept ringing.
      CallSession ended;
      if (calls_.remoteEnd(signal.callId, now, ended) != CallStatus::kOk) {
        return;
      }
      if (ended.callId[0] == '\0') {
        return;  // this station no longer tracks that call
      }
      const char* local = localParticipant(ended);
      const char* reason = callEndReasonName(static_cast<CallEndReason>(signal.reason));
      const char* event = (signal.action == kSignalReject)   ? "call_rejected"
                          : (signal.action == kSignalCancel) ? "call_cancelled"
                                                             : "call_ended";
      if (local != nullptr) {
        const char* peer = (strcmp(ended.caller, local) == 0) ? ended.callee : ended.caller;
        notifyCallEnded(local, ended.callId, peer, event, reason);
      }
      LOG_I("call", "cross cell call %s ended by node %u (%s)", ended.callId,
            static_cast<unsigned>(fromNode), reason);
      return;
    }

    default:
      LOG_D("call", "unhandled cross cell signal action %u", static_cast<unsigned>(signal.action));
      return;
  }
}

void AppServer::onLinkAudio(uint8_t* audioFrame, size_t length, uint8_t fromNode) {
  ++callAudioFromLink_;
  char callId[kCallIdLength + 1] = {0};
  uint16_t sequence = 0;
  const uint8_t* media = nullptr;
  size_t mediaLength = 0;
  if (!parseCallAudioFrame(audioFrame, length, callId, sizeof(callId), sequence, &media,
                           mediaLength)) {
    ++callAudioRejected_;
    return;
  }
  (void)media;
  (void)mediaLength;
  // The frame must belong to a cross cell session whose other station is exactly
  // the one that sent it: a station cannot inject audio into another call.
  if (calls_.remoteNodeOf(callId) != fromNode) {
    ++callAudioRejected_;
    return;
  }
  const CallSession* session = calls_.find(callId);
  if (session == nullptr || session->state != CallState::kActive) {
    ++callAudioRejected_;
    return;
  }
  const char* local = localParticipant(*session);
  if (local == nullptr) {
    ++callAudioRejected_;
    return;
  }
  // Same delivery path as a local call: the binary media frame goes to the local
  // participant's WebSocket, unchanged.
  size_t delivered = 0;
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (socketUsers_[i][0] == '\0') {
      continue;
    }
    if (strcmp(socketUsers_[i], local) != 0) {
      continue;
    }
    if (ws_.sendBIN(i, audioFrame, length)) {
      ++delivered;
    }
  }
  if (delivered == 0U) {
    ++callAudioSilent_;
    return;
  }
  calls_.noteMedia(callId, millis());
  callAudioRelayed_ += static_cast<uint32_t>(delivered);
}

// ---------------------------------------------------------------------------
// Real time calls - lifecycle helpers
// ---------------------------------------------------------------------------
void AppServer::serviceCallTimeouts() {
  const uint32_t now = millis();
  // One expired session per call: bounded work, never a blocking loop.
  CallSession expired;
  while (calls_.expireRinging(now, expired)) {
    LOG_I("call", "invitation %s expired without an answer", expired.callId);
    broadcastCallEvent("call_timeout", expired.callId, "timeout");
    if (expired.remoteNode != 0U) {
      // The other station must release its half of the invitation too, otherwise
      // its phone would ring for a call nobody is waiting for.
      LinkCallSignal signal;
      copyField(signal.callId, sizeof(signal.callId), expired.callId);
      signal.action = kSignalCancel;
      signal.reason = static_cast<uint8_t>(CallEndReason::kTimeout);
      copyField(signal.from, sizeof(signal.from), expired.caller);
      copyField(signal.to, sizeof(signal.to), expired.callee);
      if (link_.sendSignal(signal, expired.remoteNode)) {
        ++callSignalsSent_;
      }
    }
  }
  while (calls_.expireStaleMedia(now, expired)) {
    LOG_W("call", "call %s dropped: no media for %lu ms", expired.callId,
          static_cast<unsigned long>(kCallMediaTimeoutMs));
    notifyCallEnded(expired.caller, expired.callId, expired.callee, "call_ended", "timeout");
    notifyCallEnded(expired.callee, expired.callId, expired.caller, "call_ended", "timeout");
    if (expired.remoteNode != 0U) {
      LinkCallSignal signal;
      copyField(signal.callId, sizeof(signal.callId), expired.callId);
      signal.action = kSignalEnded;
      signal.reason = static_cast<uint8_t>(CallEndReason::kTimeout);
      copyField(signal.from, sizeof(signal.from), expired.caller);
      copyField(signal.to, sizeof(signal.to), expired.callee);
      if (link_.sendSignal(signal, expired.remoteNode)) {
        ++callSignalsSent_;
      }
    }
  }
}

void AppServer::notifyCallEnded(const char* userId,
                                const char* callId,
                                const char* peer,
                                const char* event,
                                const char* reason) {
  if (userId == nullptr || userId[0] == '\0') {
    return;
  }
  String out;
  out.reserve(160);
  out += "{\"type\":\"";
  out += event;
  out += "\",\"callId\":\"";
  appendJsonEscaped(out, callId);
  out += "\",\"peer\":\"";
  appendJsonEscaped(out, peer);
  out += "\",\"cell\":\"";
  out += config_.siteId;
  out += "\",\"state\":\"ended\",\"reason\":\"";
  out += reason;
  out += "\"}";
  pushToUser(userId, out);
}

void AppServer::broadcastCallEvent(const char* event, const char* callId, const char* reason) {
  String out;
  out.reserve(160);
  out += "{\"type\":\"";
  out += event;
  out += "\",\"callId\":\"";
  appendJsonEscaped(out, callId);
  out += "\",\"cell\":\"";
  out += config_.siteId;
  out += "\",\"state\":\"ended\",\"reason\":\"";
  out += reason;
  out += "\"}";
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (socketUsers_[i][0] != '\0') {
      ws_.sendTXT(i, out.c_str());
      ++pushedEvents_;
    }
  }
}

// Called when a socket closes: the session of that user is released and the
// other participant is told, so no audio resource stays open.
void AppServer::endCallsForUser(const char* userId, const char* event, const char* reason) {
  CallSession ended;
  if (calls_.endForUser(userId, CallEndReason::kDisconnected, millis(), ended) !=
      CallStatus::kOk) {
    return;
  }
  const char* peer = (strcmp(ended.caller, userId) == 0) ? ended.callee : ended.caller;
  LOG_I("call", "call %s released: %s left the cell", ended.callId, userId);
  notifyCallEnded(peer, ended.callId, userId, event, reason);
  if (ended.remoteNode != 0U) {
    // The other cell must release its half: a cross cell call cannot outlive one
    // of its participants.
    LinkCallSignal signal;
    copyField(signal.callId, sizeof(signal.callId), ended.callId);
    signal.action = kSignalEnded;
    signal.reason = static_cast<uint8_t>(CallEndReason::kDisconnected);
    copyField(signal.from, sizeof(signal.from), ended.caller);
    copyField(signal.to, sizeof(signal.to), ended.callee);
    if (link_.sendSignal(signal, ended.remoteNode)) {
      ++callSignalsSent_;
    }
  }
}

void AppServer::replyCallStatus(CallStatus status) {
  switch (status) {
    case CallStatus::kOk:
      sendStatus(200, "ok");
      return;
    case CallStatus::kSelfCall:
      sendStatus(400, "self-call", "a user cannot call itself");
      return;
    case CallStatus::kNotLocal:
      // The participant is not connected to *this* station. A cross cell call is
      // routed over the Wi-Fi backbone before reaching the table, so getting here
      // means the user really is absent.
      sendStatus(409, "unavailable",
                 "the participant is not connected to this station");
      return;
    case CallStatus::kUnknownUser:
      sendStatus(404, "unknown-user");
      return;
    case CallStatus::kUnavailable:
      sendStatus(409, "unavailable", "the recipient is not reachable on this cell");
      return;
    case CallStatus::kBusy:
      sendStatus(409, "busy", "one of the two users is already in a call");
      return;
    case CallStatus::kUnknownCall:
      sendStatus(404, "unknown-call", "no such call on this cell");
      return;
    case CallStatus::kNotParticipant:
      sendStatus(403, "not-participant", "this user is not part of the call");
      return;
    case CallStatus::kInvalidState:
      sendStatus(409, "invalid-state", "the call is not in a state allowing this action");
      return;
    case CallStatus::kExpired:
      sendStatus(410, "expired", "the invitation expired");
      return;
    case CallStatus::kTableFull:
      sendStatus(503, "call-table-full", "too many calls on this cell, retry later");
      return;
    case CallStatus::kTooSoon:
      sendStatus(429, "call-too-soon", "wait before placing another call");
      return;
    case CallStatus::kInvalidCallId:
      sendStatus(400, "invalid-call-id", "a call id is 8 alphanumeric characters");
      return;
  }
  sendStatus(500, "internal-error");
}

void AppServer::pushCallEvent(const char* userId,
                              const char* event,
                              const CallSession& session,
                              const char* reason,
                              const char* extraJson) {
  if (userId == nullptr || userId[0] == '\0') {
    return;
  }
  String out;
  out.reserve(256);
  out += "{\"type\":\"";
  out += event;
  out += "\",\"callId\":\"";
  appendJsonEscaped(out, session.callId);
  out += "\",\"from\":\"";
  appendJsonEscaped(out, session.caller);
  out += "\",\"to\":\"";
  appendJsonEscaped(out, session.callee);
  out += "\",\"cell\":\"";
  out += config_.siteId;
  out += "\",\"state\":\"";
  out += callStateName(session.state);
  out += "\",\"audio\":{\"sampleRate\":8000,\"frameMs\":40,\"frameBytes\":";
  out += String(static_cast<unsigned>(kCallAudioFrameBytes));
  out += ",\"headerBytes\":";
  out += String(static_cast<unsigned>(kCallAudioHeaderSize));
  out += "}";
  if (reason != nullptr) {
    out += ",\"reason\":\"";
    out += reason;
    out += '"';
  }
  if (extraJson != nullptr) {
    out += ',';
    out += extraJson;
  }
  out += '}';
  pushToUser(userId, out);
}

// ---------------------------------------------------------------------------
// Real time calls - local membership
// ---------------------------------------------------------------------------
bool AppServer::hasLocalSocket(const char* userId) const {
  if (userId == nullptr || userId[0] == '\0') {
    return false;
  }
  for (uint8_t i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; ++i) {
    if (socketUsers_[i][0] != '\0' && strcmp(socketUsers_[i], userId) == 0) {
      return true;
    }
  }
  return false;
}

bool AppServer::probeLocalUser(void* context, const char* userId) {
  AppServer* server = static_cast<AppServer*>(context);
  if (server == nullptr) {
    return false;
  }
  return server->hasLocalSocket(userId);
}

void AppServer::handleNotFound() {
  sendStatus(404, "not-found", http_.uri().c_str());
}

void AppServer::replyRouteStatus(MeshNode::RouteStatus status, const char* targetUser) {
  switch (status) {
    case MeshNode::RouteStatus::kQueued: {
      const RouteEntry* entry = mesh_.routing().find(targetUser);
      JsonDocument doc;
      doc["status"] = "queued";
      doc["route"] = siteNameForNode(entry != nullptr ? entry->station : 0U);
      String out;
      serializeJson(doc, out);
      sendJson(202, out);
      return;
    }
    case MeshNode::RouteStatus::kDeliveredLocally:
      sendStatus(200, "delivered-local");
      return;
    case MeshNode::RouteStatus::kUnknownRecipient:
      sendStatus(409, "unknown-recipient",
                 "no presence information for this user yet");
      return;
    case MeshNode::RouteStatus::kRecipientOffline:
      sendStatus(409, "recipient-offline",
                 "keep the message on the phone and retry later");
      return;
    case MeshNode::RouteStatus::kQueueFull:
      sendStatus(503, "busy", "RS485 egress queue full, retry later");
      return;
    case MeshNode::RouteStatus::kTooLarge:
      sendStatus(413, "too-large", "payload exceeds the fragmentation limit");
      return;
  }
  sendStatus(500, "internal-error");
}

void AppServer::handleText() {
  const String body = http_.arg("plain");
  if (body.length() == 0U) {
    sendStatus(400, "empty-body");
    return;
  }
  if (body.length() > kMaxReassembledPayload - 80U) {
    sendStatus(413, "too-large");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* id = doc["id"] | "";
  const char* from = doc["from"] | "";
  const char* to = doc["to"] | "";
  if (id[0] == '\0' || from[0] == '\0' || to[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  mesh_.refreshLocalUser(from);
  const MeshNode::RouteStatus status =
      mesh_.submitAppPayload(kTypeText, reinterpret_cast<const uint8_t*>(body.c_str()),
                             body.length(), from, to, FrameQueue::kPrioText, true);
  if (status == MeshNode::RouteStatus::kQueued) {
    LOG_I("app", "text %s routed %s -> %s (%u bytes)", id, from, to,
          static_cast<unsigned>(body.length()));
  }
  replyRouteStatus(status, to);
}

void AppServer::handleReceipt() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* id = doc["id"] | "";
  const char* from = doc["from"] | "";
  const char* to = doc["to"] | "";
  if (id[0] == '\0' || from[0] == '\0' || to[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  const MeshNode::RouteStatus status =
      mesh_.submitAppPayload(kTypeReceipt, reinterpret_cast<const uint8_t*>(body.c_str()),
                             body.length(), from, to, FrameQueue::kPrioReceipt, true);
  replyRouteStatus(status, to);
}

void AppServer::handleVoiceBlock() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* msgId = doc["msgId"] | "";
  const char* from = doc["from"] | "";
  const char* to = doc["to"] | "";
  const uint32_t seq = doc["seq"] | 0U;
  const uint32_t total = doc["total"] | 0U;
  const uint32_t crc = doc["crc"] | 0U;
  const char* data = doc["data"] | "";

  if (strlen(msgId) < 16U || from[0] == '\0' || to[0] == '\0' || data[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  if (total == 0U || seq >= total) {
    sendStatus(400, "invalid-sequence");
    return;
  }

  uint8_t block[kVoiceHeaderSize + kMaxVoiceChunk];
  memset(block, 0, sizeof(block));
  memcpy(block, msgId, 16);
  block[16] = static_cast<uint8_t>(seq & 0xFFU);
  block[17] = static_cast<uint8_t>((seq >> 8) & 0xFFU);
  block[18] = static_cast<uint8_t>(total & 0xFFU);
  block[19] = static_cast<uint8_t>((total >> 8) & 0xFFU);
  block[20] = static_cast<uint8_t>(crc & 0xFFU);
  block[21] = static_cast<uint8_t>((crc >> 8) & 0xFFU);

  const size_t dataLength =
      base64Decode(data, strlen(data), block + kVoiceHeaderSize, kMaxVoiceChunk);
  if (dataLength == 0U) {
    sendStatus(400, "empty-block");
    return;
  }

  mesh_.refreshLocalUser(from);
  const MeshNode::RouteStatus status =
      mesh_.submitAppPayload(kTypeVoiceBlock, block, kVoiceHeaderSize + dataLength, from, to,
                             FrameQueue::kPrioAudio, false);
  replyRouteStatus(status, to);
}

void AppServer::handleVoiceFeedback() {
  const String body = http_.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body.c_str(), body.length())) {
    sendStatus(400, "invalid-json");
    return;
  }
  const char* from = doc["from"] | "";
  const char* to = doc["to"] | "";
  const char* kind = doc["kind"] | "";
  if (from[0] == '\0' || to[0] == '\0' || kind[0] == '\0') {
    sendStatus(400, "missing-fields");
    return;
  }
  const MeshNode::RouteStatus status =
      mesh_.submitAppPayload(kTypeVoiceFeedback, reinterpret_cast<const uint8_t*>(body.c_str()),
                             body.length(), from, to, FrameQueue::kPrioReceipt, true);
  replyRouteStatus(status, to);
}

}  // namespace cell
