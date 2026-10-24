#include "station_link.h"

#include <lwip/sockets.h>

#include <cstring>

#include "log.h"

namespace cell {
namespace {

constexpr uint32_t kLinkHeartbeatMs = 5000;
constexpr uint32_t kLinkRouteStateMs = 10000;
constexpr uint32_t kLinkTimeoutMs = 15000;
// WiFiClient::connect() is synchronous and blocks in select() for the whole
// timeout. It is therefore kept short: the RS485 receive buffer only holds about
// 355 ms of bus traffic, so the main loop must never be stalled for longer.
constexpr uint32_t kLinkConnectTimeoutMs = 200;
// Progressive backoff of the reconnection attempts, capped so a station that
// cannot reach its upstream still retries regularly.
constexpr uint32_t kLinkMaxConnectDelayMs = 30000;
// Bounded work per socket and per loop iteration: the main loop must keep
// serving Wi-Fi, HTTP, WebSocket and the RS485 bus. One second of call audio is
// 25 frames of 668 bytes, so this budget is far above the real arrival rate.
constexpr size_t kLinkReadBudgetBytes = 6U * kLinkMaxFrameSize;

// Call audio may be dropped when the socket is congested (a late audio frame is
// useless); every other type is control traffic and must be queued.
bool isControlFrame(uint8_t type) {
  return type != kLinkCallAudio;
}

}  // namespace

StationLink::StationLink(WifiBackbone& wifi) : wifi_(wifi) {}

void StationLink::begin(const StationConfig& config,
                        SignalHandler signalHandler,
                        AudioHandler audioHandler,
                        void* context) {
  config_ = &config;
  signalHandler_ = signalHandler;
  audioHandler_ = audioHandler;
  context_ = context;

  routes_.begin(config.nodeId);
  if (config.upstreamNode != 0U) {
    routes_.setUpstream(config.upstreamNode);
  }
  upstream_.begin(millis(), config.linkRetryMs);

  server_.begin(config.linkPort);
  LOG_I("link", "inter-station link listening on %s:%u (node %u, upstream %u, channel %u)",
        config.apIp, static_cast<unsigned>(config.linkPort), static_cast<unsigned>(config.nodeId),
        static_cast<unsigned>(config.upstreamNode), static_cast<unsigned>(config.wifiChannel));
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------
StationLink::Socket& StationLink::outgoingSlot() {
  return sockets_[kSocketSlots - 1U];
}

StationLink::Socket* StationLink::freeSlot() {
  // The last slot belongs to the upstream connection, so it is not offered to an
  // accepted connection.
  for (size_t i = 0; i + 1U < kSocketSlots; ++i) {
    if (!sockets_[i].active) {
      return &sockets_[i];
    }
  }
  return nullptr;
}

StationLink::Socket* StationLink::slotForNode(uint8_t node) {
  if (node == 0U) {
    return nullptr;
  }
  for (size_t i = 0; i < kSocketSlots; ++i) {
    if (sockets_[i].active && sockets_[i].authenticated && sockets_[i].node == node) {
      return &sockets_[i];
    }
  }
  return nullptr;
}

void StationLink::closeSlot(Socket& slot, const char* reason) {
  if (!slot.active) {
    return;
  }
  if (slot.node != 0U) {
    routes_.setDirect(slot.node, false);
    // The routes learned through this peer are no longer valid: dropping them is
    // what prevents a black hole after a link loss.
    routes_.forgetVia(slot.node);
    LOG_I("link", "link with node %u closed (%s)", static_cast<unsigned>(slot.node),
          reason == nullptr ? "?" : reason);
  }
  if (slot.outgoing) {
    upstream_.onDisconnected(millis());
  }
  slot.client.stop();
  slot.active = false;
  slot.outgoing = false;
  slot.authenticated = false;
  slot.node = 0;
  slot.assembler.reset();
  slot.control.clear();
  slot.audio.clear();
  ++disconnects_;
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------
void StationLink::acceptConnections(uint32_t nowMs) {
  WiFiClient candidate = server_.available();
  if (!candidate) {
    return;
  }
  Socket* slot = freeSlot();
  if (slot == nullptr) {
    ++authRejections_;
    LOG_W("link", "no free link slot, refusing %s", candidate.remoteIP().toString().c_str());
    candidate.stop();
    return;
  }
  slot->client = candidate;
  slot->client.setNoDelay(true);
  slot->client.setTimeout(0);
  slot->active = true;
  slot->outgoing = false;
  slot->authenticated = false;
  slot->node = 0;
  slot->assembler.reset();
  slot->control.clear();
  slot->audio.clear();
  slot->liveness.begin(nowMs);
  LOG_I("link", "incoming link connection from %s, waiting for the hello",
        slot->client.remoteIP().toString().c_str());
}

void StationLink::serviceUpstream(uint32_t nowMs) {
  if (config_ == nullptr || config_->upstreamNode == 0U) {
    return;  // this station is the hub
  }
  // The TCP link cannot exist before the station interface is attached.
  if (!wifi_.upstreamReady()) {
    return;
  }
  Socket& slot = outgoingSlot();
  if (slot.active) {
    return;
  }
  if (!upstream_.shouldAttempt(nowMs)) {
    return;
  }
  upstream_.noteAttempt(nowMs);

  IPAddress address;
  if (!address.fromString(config_->upstreamIp)) {
    LOG_E("link", "invalid upstream address \"%s\"", config_->upstreamIp);
    return;
  }
  LOG_I("link", "connecting to the upstream station %s:%u", config_->upstreamIp,
        static_cast<unsigned>(config_->linkPort));
  if (!slot.client.connect(address, config_->linkPort, kLinkConnectTimeoutMs)) {
    LOG_W("link", "upstream %s:%u unreachable", config_->upstreamIp,
          static_cast<unsigned>(config_->linkPort));
    slot.client.stop();
    // Progressive backoff: the connect() call is synchronous, so a station that
    // cannot reach its upstream must not retry it every two seconds for ever.
    connectDelayMs_ = (connectDelayMs_ == 0U) ? config_->linkRetryMs : connectDelayMs_ * 2U;
    if (connectDelayMs_ > kLinkMaxConnectDelayMs) {
      connectDelayMs_ = kLinkMaxConnectDelayMs;
    }
    upstream_.setRetryDelay(connectDelayMs_);
    return;
  }
  slot.client.setNoDelay(true);
  slot.client.setTimeout(0);
  slot.active = true;
  slot.outgoing = true;
  slot.authenticated = false;
  slot.node = 0;
  slot.assembler.reset();
  slot.control.clear();
  slot.audio.clear();
  slot.liveness.begin(nowMs);
  // The upstream answered: the backoff starts over.
  connectDelayMs_ = 0;
  upstream_.setRetryDelay(config_->linkRetryMs);

  uint8_t payload[kLinkHelloPayloadSize] = {0};
  const size_t payloadLength =
      buildLinkHello(config_->nodeId, config_->linkToken, payload, sizeof(payload));
  if (payloadLength == 0U) {
    closeSlot(slot, "invalid hello payload");
    return;
  }
  LinkFrame hello;
  hello.type = kLinkHello;
  hello.srcNode = config_->nodeId;
  hello.dstNode = config_->upstreamNode;
  hello.hops = 0;
  memcpy(hello.payload, payload, payloadLength);
  hello.payloadLength = static_cast<uint16_t>(payloadLength);
  if (!enqueueFrame(slot, hello, true)) {
    closeSlot(slot, "hello queue full");
    return;
  }
  LOG_I("link", "hello sent to node %u, waiting for the acknowledgement",
        static_cast<unsigned>(config_->upstreamNode));
}

void StationLink::sendRouteState(Socket& slot) {
  if (!slot.active || !slot.authenticated) {
    return;
  }
  LinkFrame state;
  state.type = kLinkRouteState;
  state.srcNode = config_->nodeId;
  state.dstNode = slot.node;
  state.hops = 0;
  state.payload[0] = routes_.reachableMask();
  state.payloadLength = static_cast<uint16_t>(kLinkRouteStatePayloadSize);
  enqueueFrame(slot, state, true);
}

void StationLink::sendHeartbeats(uint32_t nowMs) {
  const bool routeStateDue = static_cast<uint32_t>(nowMs - lastRouteStateMs_) >= kLinkRouteStateMs;
  if (static_cast<uint32_t>(nowMs - lastHeartbeatMs_) < kLinkHeartbeatMs) {
    return;
  }
  lastHeartbeatMs_ = nowMs;
  if (routeStateDue) {
    lastRouteStateMs_ = nowMs;
  }
  for (size_t i = 0; i < kSocketSlots; ++i) {
    Socket& slot = sockets_[i];
    if (!slot.active || !slot.authenticated) {
      continue;
    }
    if (routeStateDue) {
      // Announced before the heartbeat: it is what makes a chain topology
      // converge without waiting for real traffic.
      sendRouteState(slot);
    }
    LinkFrame beat;
    beat.type = kLinkHeartbeat;
    beat.srcNode = config_->nodeId;
    beat.dstNode = slot.node;
    beat.hops = 0;
    if (!enqueueFrame(slot, beat, true)) {
      closeSlot(slot, "heartbeat queue full");
    }
  }
}

// One non blocking write. MSG_DONTWAIT never waits, so a congested socket cannot
// stall the main loop: the caller is the loop that also has to drain the RS485
// UART before its buffer overflows.
size_t StationLink::writeSocket(void* context, const uint8_t* data, size_t length) {
  WiFiClient* client = static_cast<WiFiClient*>(context);
  if (client == nullptr || data == nullptr || length == 0U) {
    return 0U;
  }
  const int fd = client->fd();
  if (fd < 0) {
    return 0U;
  }
  const int written = send(fd, data, length, MSG_DONTWAIT);
  if (written <= 0) {
    return 0U;
  }
  return static_cast<size_t>(written);
}

// Drains at most one slice of each ring. The control ring is served first so a
// call signal is never delayed behind call audio.
void StationLink::flushSocket(Socket& slot) {
  if (!slot.active) {
    return;
  }
  if (!slot.control.empty()) {
    slot.control.flush(&StationLink::writeSocket, &slot.client);
  }
  if (!slot.audio.empty()) {
    slot.audio.flush(&StationLink::writeSocket, &slot.client);
  }
}

bool StationLink::enqueueFrame(Socket& slot, LinkFrame& frame, bool control) {
  if (!slot.active || !slot.client.connected()) {
    return false;
  }
  uint8_t encoded[kLinkMaxFrameSize];
  const size_t length = encodeLinkFrame(frame, encoded, sizeof(encoded));
  if (length == 0U) {
    return false;
  }
  LinkTxBuffer& target = control ? slot.control : slot.audio;
  if (target.push(encoded, length) == 0U) {
    // The ring is full: try to free it immediately, then give up. Nothing is
    // ever written partially, so the peer stream stays valid.
    flushSocket(slot);
    if (target.push(encoded, length) == 0U) {
      return false;
    }
  }
  ++framesSent_;
  return true;
}

// ---------------------------------------------------------------------------
// Reception
// ---------------------------------------------------------------------------
void StationLink::serviceSocket(Socket& slot, uint32_t nowMs) {
  if (!slot.active) {
    return;
  }
  if (!slot.client.connected()) {
    closeSlot(slot, "socket closed by the peer");
    return;
  }
  uint8_t chunk[512] = {0};
  // Declared once: a LinkFrame is ~720 bytes and value initialised, so building
  // one per received byte would burn a lot of time in the main loop.
  LinkFrame frame;
  size_t budget = kLinkReadBudgetBytes;
  while (budget > 0U && slot.client.available() > 0) {
    const size_t want = (budget < sizeof(chunk)) ? budget : sizeof(chunk);
    const int got = slot.client.read(chunk, want);
    if (got <= 0) {
      break;
    }
    budget -= static_cast<size_t>(got);
    // Bytes really arrived: the peer is alive, whatever the frame contains.
    slot.liveness.noteActivity(nowMs);
    for (int i = 0; i < got; ++i) {
      if (slot.assembler.push(chunk[i], frame) == LinkStreamAssembler::Status::kFrameReady) {
        handleFrame(slot, frame, nowMs);
        if (!slot.active) {
          return;  // the handler closed this socket
        }
      }
    }
  }
  // Outbound: at most one non blocking slice per ring, so this can never block.
  flushSocket(slot);
  if (slot.active && slot.authenticated && slot.liveness.stale(nowMs, kLinkTimeoutMs)) {
    LOG_W("link", "node %u silent for %lu ms, closing the link", static_cast<unsigned>(slot.node),
          static_cast<unsigned long>(kLinkTimeoutMs));
    closeSlot(slot, "heartbeat timeout");
  }
}

void StationLink::handleFrame(Socket& slot, LinkFrame& frame, uint32_t nowMs) {
  ++framesReceived_;
  if (config_ == nullptr) {
    return;
  }

  if (frame.type == kLinkHello) {
    if (slot.outgoing) {
      ++authRejections_;
      closeSlot(slot, "hello on an outgoing socket");
      return;
    }
    uint8_t nodeId = 0;
    char token[kLinkTokenLength] = {0};
    if (!parseLinkHello(frame.payload, frame.payloadLength, nodeId, token, sizeof(token))) {
      ++authRejections_;
      closeSlot(slot, "malformed hello");
      return;
    }
    const char* expected = config_->linkToken;
    const size_t expectedLength = (expected == nullptr) ? 0U : strlen(expected);
    if (!linkTokenMatches(expected, expectedLength, token)) {
      ++authRejections_;
      LOG_W("link", "rejected node %u from %s: invalid token", static_cast<unsigned>(nodeId),
            slot.client.remoteIP().toString().c_str());
      // The refusal carries no information beyond "refused".
      LinkFrame reject;
      reject.type = kLinkReject;
      reject.srcNode = config_->nodeId;
      reject.dstNode = nodeId;
      reject.hops = 0;
      enqueueFrame(slot, reject, true);
      // Give the refusal a chance to leave before the socket is closed.
      flushSocket(slot);
      closeSlot(slot, "authentication failed");
      return;
    }
    if (nodeId == config_->nodeId) {
      ++authRejections_;
      closeSlot(slot, "a station cannot link to itself");
      return;
    }
    // One socket per peer: a new connection replaces the previous one.
    Socket* existing = slotForNode(nodeId);
    if (existing != nullptr && existing != &slot) {
      closeSlot(*existing, "replaced by a new connection");
    }
    slot.node = nodeId;
    slot.authenticated = true;
    routes_.setDirect(nodeId, true);

    LinkFrame ack;
    ack.type = kLinkHelloAck;
    ack.srcNode = config_->nodeId;
    ack.dstNode = nodeId;
    ack.hops = 0;
    enqueueFrame(slot, ack, true);
    LOG_I("link", "node %u authenticated from %s", static_cast<unsigned>(nodeId),
          slot.client.remoteIP().toString().c_str());
    // The new peer immediately learns what this station can reach.
    sendRouteState(slot);
    return;
  }

  if (frame.type == kLinkHelloAck) {
    if (!slot.outgoing || frame.srcNode != config_->upstreamNode) {
      ++authRejections_;
      closeSlot(slot, "unexpected hello-ack");
      return;
    }
    slot.node = frame.srcNode;
    slot.authenticated = true;
    routes_.setDirect(frame.srcNode, true);
    upstream_.onConnected(nowMs);
    LOG_I("link", "backbone up: attached to node %u (%s)", static_cast<unsigned>(frame.srcNode),
          slot.client.remoteIP().toString().c_str());
    sendRouteState(slot);
    return;
  }

  if (frame.type == kLinkReject) {
    ++authRejections_;
    LOG_E("link", "the upstream station refused the link (invalid token?)");
    closeSlot(slot, "refused by the peer");
    return;
  }

  if (!slot.authenticated) {
    // Nothing is processed before the handshake.
    ++framesDropped_;
    closeSlot(slot, "frame received before authentication");
    return;
  }

  // Any frame coming from an authenticated socket proves that its original source
  // is reachable through that socket: a chain topology converges from real
  // traffic as well as from the periodic announcements.
  routes_.learnVia(frame.srcNode, slot.node);

  if (frame.type == kLinkRouteState) {
    if (frame.payloadLength >= kLinkRouteStatePayloadSize) {
      routes_.applyReachableMask(frame.payload[0], slot.node);
    } else {
      ++framesDropped_;
    }
    return;
  }

  if (frame.type == kLinkHeartbeat) {
    return;
  }

  if (frame.type == kLinkCallSignal) {
    if (frame.dstNode == config_->nodeId) {
      LinkCallSignal signal;
      if (!parseLinkCallSignal(frame.payload, frame.payloadLength, signal)) {
        ++framesDropped_;
        LOG_W("link", "malformed call signal from node %u", static_cast<unsigned>(frame.srcNode));
        return;
      }
      ++signalsReceived_;
      if (signalHandler_ != nullptr) {
        signalHandler_(context_, signal, frame.srcNode);
      }
      return;
    }
    relayFrame(slot, frame);
    return;
  }

  if (frame.type == kLinkCallAudio) {
    if (frame.dstNode == config_->nodeId) {
      ++audioReceived_;
      if (audioHandler_ != nullptr) {
        audioHandler_(context_, frame.payload, frame.payloadLength, frame.srcNode);
      }
      return;
    }
    relayFrame(slot, frame);
    return;
  }

  ++framesDropped_;
  LOG_D("link", "unknown link frame type 0x%02X from node %u", static_cast<unsigned>(frame.type),
        static_cast<unsigned>(frame.srcNode));
}

void StationLink::relayFrame(Socket& incoming, LinkFrame& frame) {
  const uint8_t target = routes_.relayTarget(frame.dstNode, incoming.node, frame.hops);
  if (target == 0U) {
    ++framesDropped_;
    LOG_D("link", "dropped frame for node %u (no route, loop or too many hops)",
          static_cast<unsigned>(frame.dstNode));
    return;
  }
  Socket* next = slotForNode(target);
  if (next == nullptr) {
    ++framesDropped_;
    return;
  }
  // The original source is preserved: the destination station must know which
  // station really hosts the remote participant.
  if (frame.hops < 255U) {
    ++frame.hops;
  }
  if (!enqueueFrame(*next, frame, isControlFrame(frame.type))) {
    ++framesDropped_;
    return;
  }
  ++framesRelayed_;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
bool StationLink::sendSignal(const LinkCallSignal& signal, uint8_t dstNode) {
  if (config_ == nullptr || dstNode == 0U || dstNode == config_->nodeId) {
    // A local call never uses the backbone.
    return false;
  }
  uint8_t payload[kLinkCallSignalSize] = {0};
  const size_t length = buildLinkCallSignal(signal, payload, sizeof(payload));
  if (length == 0U) {
    return false;
  }
  const uint8_t next = routes_.nextHop(dstNode);
  if (next == 0U) {
    ++framesDropped_;
    return false;
  }
  Socket* slot = slotForNode(next);
  if (slot == nullptr) {
    ++framesDropped_;
    return false;
  }
  LinkFrame frame;
  frame.type = kLinkCallSignal;
  frame.srcNode = config_->nodeId;
  frame.dstNode = dstNode;
  frame.hops = 0;
  memcpy(frame.payload, payload, length);
  frame.payloadLength = static_cast<uint16_t>(length);
  if (!enqueueFrame(*slot, frame, true)) {
    ++framesDropped_;
    return false;
  }
  ++signalsSent_;
  return true;
}

bool StationLink::sendAudio(const uint8_t* audioFrame, size_t length, uint8_t dstNode) {
  if (config_ == nullptr || dstNode == 0U || dstNode == config_->nodeId || audioFrame == nullptr ||
      length == 0U || length > kLinkMaxPayload) {
    return false;
  }
  const uint8_t next = routes_.nextHop(dstNode);
  if (next == 0U) {
    ++framesDropped_;
    return false;
  }
  Socket* slot = slotForNode(next);
  if (slot == nullptr) {
    ++framesDropped_;
    return false;
  }
  LinkFrame frame;
  frame.type = kLinkCallAudio;
  frame.srcNode = config_->nodeId;
  frame.dstNode = dstNode;
  frame.hops = 0;
  memcpy(frame.payload, audioFrame, length);
  frame.payloadLength = static_cast<uint16_t>(length);
  if (!enqueueFrame(*slot, frame, false)) {
    ++framesDropped_;
    return false;
  }
  ++audioSent_;
  return true;
}

// ---------------------------------------------------------------------------
// Loop and diagnostics
// ---------------------------------------------------------------------------
void StationLink::loop(uint32_t nowMs) {
  if (config_ == nullptr) {
    return;
  }
  const uint32_t startUs = micros();
  acceptConnections(nowMs);
  serviceUpstream(nowMs);
  for (size_t i = 0; i < kSocketSlots; ++i) {
    serviceSocket(sockets_[i], nowMs);
  }
  sendHeartbeats(nowMs);
  const uint32_t elapsedUs = micros() - startUs;
  if (elapsedUs > maxLoopUs_) {
    maxLoopUs_ = elapsedUs;
  }
}

size_t StationLink::txPending() const {
  size_t pending = 0;
  for (size_t i = 0; i < kSocketSlots; ++i) {
    pending += sockets_[i].control.size();
    pending += sockets_[i].audio.size();
  }
  return pending;
}

bool StationLink::reachable(uint8_t node) const {
  return routes_.reachable(node);
}

bool StationLink::hasPeer(uint8_t node) const {
  return const_cast<StationLink*>(this)->slotForNode(node) != nullptr;
}

bool StationLink::upstreamReady() const {
  return wifi_.upstreamReady();
}

void StationLink::logStatus(const char* reason) const {
  LOG_I("link",
        "[%s] node=%u peers=0x%X upstream=%u up=%u | tx=%lu rx=%lu relay=%lu drop=%lu | "
        "sig=%lu/%lu audio=%lu/%lu reject=%lu disc=%lu",
        reason == nullptr ? "?" : reason,
        static_cast<unsigned>(config_ == nullptr ? 0 : config_->nodeId),
        static_cast<unsigned>(routes_.directMask()),
        static_cast<unsigned>(config_ == nullptr ? 0 : config_->upstreamNode),
        upstream_.connected() ? 1U : 0U, static_cast<unsigned long>(framesSent_),
        static_cast<unsigned long>(framesReceived_), static_cast<unsigned long>(framesRelayed_),
        static_cast<unsigned long>(framesDropped_), static_cast<unsigned long>(signalsSent_),
        static_cast<unsigned long>(signalsReceived_), static_cast<unsigned long>(audioSent_),
        static_cast<unsigned long>(audioReceived_), static_cast<unsigned long>(authRejections_),
        static_cast<unsigned long>(disconnects_));
}

}  // namespace cell
