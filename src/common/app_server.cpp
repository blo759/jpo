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

AppServer::AppServer(MeshNode& mesh, const StationConfig& config)
    : mesh_(mesh), config_(config), http_(config.servicePort), ws_(config.wsPort) {
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
  http_.onNotFound([this]() { handleNotFound(); });
  http_.begin();

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
    case WStype_DISCONNECTED:
      socketUsers_[clientNumber][0] = '\0';
      LOG_I("ws", "socket %u disconnected", static_cast<unsigned>(clientNumber));
      break;

    case WStype_CONNECTED:
      LOG_I("ws", "socket %u connected, waiting for hello", static_cast<unsigned>(clientNumber));
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
      bindSocketToUser(clientNumber, userId);
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

void AppServer::bindSocketToUser(uint8_t clientNumber, const char* userId) {
  if (userId == nullptr || userId[0] == '\0' || strlen(userId) >= kMaxUserIdLength) {
    socketUsers_[clientNumber][0] = '\0';
    LOG_W("ws", "invalid user id on socket %u", static_cast<unsigned>(clientNumber));
    return;
  }
  memset(socketUsers_[clientNumber], 0, kMaxUserIdLength);
  strncpy(socketUsers_[clientNumber], userId, kMaxUserIdLength - 1U);
  if (!mesh_.refreshLocalUser(socketUsers_[clientNumber])) {
    mesh_.registerLocalUser(socketUsers_[clientNumber], socketUsers_[clientNumber]);
  }
  LOG_I("ws", "socket %u bound to user %s", static_cast<unsigned>(clientNumber),
        socketUsers_[clientNumber]);
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
