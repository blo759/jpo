#include "mesh_node.h"

#include "log.h"

namespace cell {
namespace {

constexpr uint32_t kPingIntervalMs = 5000;
constexpr uint32_t kPeerTimeoutMs = 15000;
constexpr uint32_t kRouteStateIntervalMs = 10000;
// First retransmission window. It must stay larger than the token claim window
// (400 ms + 40 ms per node id), otherwise every frame is retransmitted before
// the peer had any chance to answer. The AckTracker then doubles the delay after
// each attempt and only fires when the bus is really silent.
constexpr uint32_t kAckTimeoutMs = 400;
constexpr uint32_t kReassemblyTimeoutMs = 15000;
constexpr uint32_t kAudioStarvationMs = 2500;
constexpr uint32_t kAliveScanIntervalMs = 1000;
constexpr uint32_t kLocalUserTimeoutMs = 90000;
// Period of the compact bus summary printed on the serial console. One line every
// 10 s is enough to follow a bus problem and costs about 0,01 % of the link.
constexpr uint32_t kDiagIntervalMs = 10000;

}  // namespace

MeshNode::MeshNode(HardwareSerial& serial, LedIndicators& leds)
    : serial_(serial), leds_(leds), bus_(serial, kRs485UartPort, leds) {}

void MeshNode::begin(const StationConfig& config,
                     AppFrameHandler appHandler,
                     PresenceHandler presenceHandler,
                     void* context) {
  config_ = &config;
  appHandler_ = appHandler;
  presenceHandler_ = presenceHandler;
  context_ = context;
  bootMs_ = millis();

  leds_.begin(kLedTxPin, kLedRxPin);
  bus_.begin(kRs485Baud);

  BusArbiterConfig arbiterConfig;
  arbiterConfig.nodeId = config.nodeId;
  arbiterConfig.holdMs = 120;
  arbiterConfig.tokenTimeoutMs = 400;
  arbiterConfig.claimSlotMs = 40;
  arbiterConfig.maxFramesPerTurn = 4;
  arbiter_.begin(arbiterConfig, bootMs_);
  arbiter_.setAliveMask(0);

  peerSeen_[config.nodeId] = true;
  peerLastSeenMs_[config.nodeId] = bootMs_;
  lastPingMs_ = bootMs_;
  lastRouteStateMs_ = bootMs_;
  lastAliveScanMs_ = bootMs_;
  lastSummaryMs_ = bootMs_;

  LOG_I("node", "%s (node %u) ready on RS485 %lu baud, DE/RE GPIO4, termination=%s",
        config.siteId, static_cast<unsigned>(config.nodeId),
        static_cast<unsigned long>(kRs485Baud), config.rs485Terminated ? "120R" : "none");
  LOG_I("node", "wiring TX=GPIO17 RX=GPIO16 DIR=GPIO4 LED_TX=GPIO25 LED_RX=GPIO26");
  LOG_I("node", "queues: %u application + %u control frames, %u fragments x %u bytes, %u ack entries",
        static_cast<unsigned>(kQueueCapacity), static_cast<unsigned>(kControlQueueCapacity),
        static_cast<unsigned>(kMaxFragments), static_cast<unsigned>(kFragmentBodySize),
        static_cast<unsigned>(kMaxAckMessages));
}

Frame MeshNode::makeFrame(uint8_t type,
                          uint8_t destination,
                          const uint8_t* payload,
                          size_t length,
                          uint8_t flags) {
  Frame frame;
  frame.type = type;
  frame.flags = flags;
  frame.srcNode = config_->nodeId;
  frame.dstNode = destination;
  frame.seq = nextSequence_++;
  frame.payloadLength = static_cast<uint16_t>(length > kMaxBusPayload ? kMaxBusPayload : length);
  if (payload != nullptr && frame.payloadLength > 0U) {
    memcpy(frame.payload, payload, frame.payloadLength);
  }
  return frame;
}

void MeshNode::loop() {
  const uint32_t loopStartUs = micros();
  const uint32_t now = millis();
  leds_.update(now);
  bus_.update(now);

  Frame frame;
  uint8_t processed = 0;
  while (processed < 6U && bus_.poll(frame, now)) {
    lastRxMs_ = now;  // a real frame arrived: proof the peer is alive
    handleBusFrame(frame, now);
    ++processed;
  }

  servicePeerLiveness(now);
  servicePresence(now);
  serviceRetransmissions(now);
  reassembler_.expire(now, kReassemblyTimeoutMs);
  queue_.promoteStarved(now, kAudioStarvationMs);
  serviceArbitration(now);

  // Periodic bus summary: one compact line every kDiagIntervalMs. It is what
  // makes a bus problem readable on the serial console (and it is cheap: about
  // 150 bytes every 10 s, i.e. 0,01 % of the link capacity).
  if (static_cast<uint32_t>(now - lastSummaryMs_) >= kDiagIntervalMs) {
    lastSummaryMs_ = now;
    logBusSummary("periodic");
  }

  pollSerialConsole();

  const uint32_t loopUs = micros() - loopStartUs;
  if (loopUs > maxLoopUs_) {
    maxLoopUs_ = loopUs;
  }
}

// One line with everything needed to tell the six failure situations apart:
// no byte received, bytes but no frame, frames but no token, token but no route,
// route but no transmission, or a higher layer problem.
void MeshNode::logBusSummary(const char* reason) {
  uint8_t aliveMask = 0;
  for (uint8_t node = 1; node <= kMaxStationNodes; ++node) {
    if (arbiter_.isAlive(node)) {
      aliveMask = static_cast<uint8_t>(aliveMask | (1U << (node - 1U)));
    }
  }
  size_t onlineUsers = 0;
  for (size_t i = 0; i < routing_.entries(); ++i) {
    if (routing_.at(i).online) {
      ++onlineUsers;
    }
  }
  LOG_I("diag",
        "[%s] node=%u alive=0x%X token=%u idle=%u | rx=%luB/%luf tx=%luB/%luf | crc=%lu cobsErr=%lu "
        "retry=%lu fail=%lu dup=%lu | tokRx=%lu tokPass=%lu tokClaim=%lu | q=%u/%u routes=%u/%u "
        "presExp=%lu reasm=%lu | heap=%lu loop=%luus",
        reason == nullptr ? "?" : reason, static_cast<unsigned>(config_->nodeId),
        static_cast<unsigned>(aliveMask), arbiter_.hasToken() ? 1U : 0U,
        static_cast<unsigned>(arbiter_.idlePasses()),
        static_cast<unsigned long>(bus_.rxBytes()),
        static_cast<unsigned long>(bus_.framesReceived()),
        static_cast<unsigned long>(bus_.txBytes()),
        static_cast<unsigned long>(bus_.framesSent()),
        static_cast<unsigned long>(bus_.decodeErrors()),
        static_cast<unsigned long>(bus_.assemblerErrors()),
        static_cast<unsigned long>(ackTracker_.retransmissions()),
        static_cast<unsigned long>(ackTracker_.failures()),
        static_cast<unsigned long>(duplicates_),
        static_cast<unsigned long>(tokensReceived_), static_cast<unsigned long>(tokenPasses_),
        static_cast<unsigned long>(tokensClaimed_),
        static_cast<unsigned>(queue_.size()), static_cast<unsigned>(queue_.controlSize()),
        static_cast<unsigned>(routing_.entries()), static_cast<unsigned>(onlineUsers),
        static_cast<unsigned long>(presenceExpirations_),
        static_cast<unsigned long>(reassembler_.completedCount()),
        static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(maxLoopUs_));
}

// Serial console. Read only diagnostics: it never changes the protocol, the
// routing or the stored data. It exists so that a bus problem can be diagnosed
// from the USB port alone, without any Wi-Fi or HTTP access.
void MeshNode::pollSerialConsole() {
  while (Serial.available() > 0) {
    const int value = Serial.read();
    if (value < 0) {
      break;
    }
    switch (static_cast<char>(value)) {
      case 'd':
      case 'D':
        logBusSummary("console");
        break;
      case 'p':
      case 'P':
        LOG_I("diag", "peers: 1=%s 2=%s 3=%s (own=%u)",
              peerSeen_[1] ? "seen" : "silent", peerSeen_[2] ? "seen" : "silent",
              peerSeen_[3] ? "seen" : "silent", static_cast<unsigned>(config_->nodeId));
        for (size_t i = 0; i < routing_.entries(); ++i) {
          const RouteEntry& entry = routing_.at(i);
          LOG_I("diag", "  route %s -> station %u %s (%lu ms)", entry.userId,
                static_cast<unsigned>(entry.station), entry.online ? "online" : "offline",
                static_cast<unsigned long>(millis() - entry.lastSeenMs));
        }
        break;
      case 'b':
      case 'B':
        LOG_I("diag", "bus: %lu baud, de/re gpio%u, tx=%luB/%lu frames, rx=%luB/%lu frames, "
                      "crc=%lu, cobs overflow=%lu, txRejected=%lu, encodeErr=%lu",
              static_cast<unsigned long>(kRs485Baud), static_cast<unsigned>(kRs485DirPin),
              static_cast<unsigned long>(bus_.txBytes()),
              static_cast<unsigned long>(bus_.framesSent()),
              static_cast<unsigned long>(bus_.rxBytes()),
              static_cast<unsigned long>(bus_.framesReceived()),
              static_cast<unsigned long>(bus_.decodeErrors()),
              static_cast<unsigned long>(bus_.assemblerOverflows()),
              static_cast<unsigned long>(bus_.txRejected()),
              static_cast<unsigned long>(bus_.encodeErrors()));
        break;
      case 'h':
      case 'H':
      case '?':
        LOG_I("diag", "serial console: d=summary p=peers/routes b=bus counters h=help");
        break;
      default:
        break;
    }
  }
}

MeshNode::RouteStatus MeshNode::submitAppPayload(uint8_t type,
                                                 const uint8_t* body,
                                                 size_t bodyLength,
                                                 const char* fromUser,
                                                 const char* toUser,
                                                 FrameQueue::Priority priority,
                                                 bool requireAck) {
  if (config_ == nullptr) {
    return RouteStatus::kQueueFull;
  }
  const RouteEntry* entry = routing_.find(toUser);
  if (entry == nullptr) {
    return RouteStatus::kUnknownRecipient;
  }
  if (!entry->online) {
    return RouteStatus::kRecipientOffline;
  }
  const uint8_t destination = entry->station;

  const size_t payloadLength =
      buildAppEnvelope(fromUser, toUser, body, bodyLength, appPayloadBuffer_,
                       sizeof(appPayloadBuffer_));
  if (payloadLength == 0U) {
    return RouteStatus::kTooLarge;
  }

  const uint32_t now = millis();

  // Same cell: the message never needs the RS485 bus.
  if (destination == config_->nodeId) {
    deliverToApp(type, appPayloadBuffer_, payloadLength);
    return RouteStatus::kDeliveredLocally;
  }

  if (payloadLength <= kMaxBusPayload) {
    if (queue_.freeSlots() < 1U) {
      ++queueDrops_;
      LOG_W("bus", "egress queue full, %s to node %u refused (explicit back pressure)", fromUser,
            static_cast<unsigned>(destination));
      return RouteStatus::kQueueFull;
    }
    const Frame frame = makeFrame(type, destination, appPayloadBuffer_, payloadLength,
                                  requireAck ? kFlagAckRequired : 0U);
    if (!queue_.push(priority, frame, now)) {
      ++queueDrops_;
      return RouteStatus::kQueueFull;
    }
    return RouteStatus::kQueued;
  }

  const size_t fragmentCount = (payloadLength + kFragmentBodySize - 1U) / kFragmentBodySize;
  if (fragmentCount > kMaxFragments) {
    return RouteStatus::kTooLarge;
  }
  // All fragments are reserved at once: a partially queued message would never
  // be reassembled and would be lost silently.
  if (queue_.freeSlots() < fragmentCount) {
    ++queueDrops_;
    LOG_W("bus", "egress queue full, %u fragments to node %u refused", static_cast<unsigned>(fragmentCount),
          static_cast<unsigned>(destination));
    return RouteStatus::kQueueFull;
  }

  const uint16_t messageSeq = nextSequence_;
  size_t queued = 0;
  for (size_t i = 0; i < fragmentCount; ++i) {
    const size_t offset = i * kFragmentBodySize;
    const size_t remaining = payloadLength - offset;
    const size_t chunk = remaining < kFragmentBodySize ? remaining : kFragmentBodySize;

    Frame frame;
    frame.type = type;
    frame.srcNode = config_->nodeId;
    frame.dstNode = destination;
    frame.seq = nextSequence_++;
    frame.flags = static_cast<uint8_t>(kFlagFragmented | (requireAck ? kFlagAckRequired : 0U));
    frame.payload[0] = static_cast<uint8_t>(i);
    frame.payload[1] = static_cast<uint8_t>(fragmentCount);
    frame.payload[2] = static_cast<uint8_t>(messageSeq & 0xFFU);
    frame.payload[3] = static_cast<uint8_t>(messageSeq >> 8);
    memcpy(frame.payload + kFragmentOverhead, appPayloadBuffer_ + offset, chunk);
    frame.payloadLength = static_cast<uint16_t>(chunk + kFragmentOverhead);
    if (!queue_.push(priority, frame, now)) {
      ++queueDrops_;
      LOG_W("bus", "fragment %u/%u dropped (queue full)", static_cast<unsigned>(i + 1U),
            static_cast<unsigned>(fragmentCount));
      break;
    }
    ++queued;
  }
  if (queued != fragmentCount) {
    return RouteStatus::kQueueFull;
  }
  LOG_D("bus", "queued %u fragments (%u bytes) to node %u", static_cast<unsigned>(fragmentCount),
        static_cast<unsigned>(payloadLength), static_cast<unsigned>(destination));
  return RouteStatus::kQueued;
}

void MeshNode::handleBusFrame(Frame& frame, uint32_t nowMs) {
  arbiter_.onBusActivity(nowMs);

  if (frame.srcNode == config_->nodeId) {
    return;  // our own echo, must not happen with DE/RE tied
  }
  if (frame.dstNode != kNodeBroadcast && frame.dstNode != config_->nodeId) {
    return;  // frame addressed to another station
  }

  switch (frame.type) {
    case kTypePing:
    case kTypePong:
      if (markPeerSeen(frame.srcNode, nowMs)) {
        LOG_I("pres", "peer node %u online", static_cast<unsigned>(frame.srcNode));
      }
      break;

    case kTypeToken:
      if (frame.payloadLength >= 2U && frame.payload[0] == config_->nodeId) {
        arbiter_.takeToken(nowMs, frame.payload[1]);
        tokenFromNode_ = frame.srcNode;
        ++tokensReceived_;
      }
      break;

    case kTypeRouteState:
      if (routing_.applyStationState(frame.srcNode, frame.payload, frame.payloadLength, nowMs)) {
        presenceDirty_ = true;
        if (presenceHandler_ != nullptr) {
          presenceHandler_(context_);
        }
      } else {
        LOG_W("pres", "invalid route state from node %u", static_cast<unsigned>(frame.srcNode));
      }
      break;

    case kTypeRouteRequest:
      presenceDirty_ = true;
      break;

    case kTypeBusAck:
      if (frame.payloadLength >= 2U) {
        const uint16_t acked = static_cast<uint16_t>(frame.payload[0]) |
                               (static_cast<uint16_t>(frame.payload[1]) << 8);
        if (!ackTracker_.onAck(frame.srcNode, acked)) {
          LOG_D("bus", "unexpected ack seq=%u from node %u", static_cast<unsigned>(acked),
                static_cast<unsigned>(frame.srcNode));
        }
      }
      break;

    default:
      handleApplicationFrame(frame, nowMs);
      break;
  }

  // The last data frame of a turn carries the token: the peer may transmit next.
  if ((frame.flags & kFlagTokenPass) != 0U && frame.dstNode == config_->nodeId) {
    arbiter_.takeToken(nowMs, 0);
    tokenFromNode_ = frame.srcNode;
  }
}

void MeshNode::handleApplicationFrame(const Frame& frame, uint32_t nowMs) {
  if (isDuplicate(frame.srcNode, frame.seq)) {
    ++duplicates_;
    LOG_D("bus", "duplicate %s seq=%u from node %u ignored", messageTypeName(frame.type),
          static_cast<unsigned>(frame.seq), static_cast<unsigned>(frame.srcNode));
    if ((frame.flags & kFlagAckRequired) != 0U) {
      sendBusAck(frame.srcNode, frame.seq, nowMs);
    }
    return;
  }
  rememberSequence(frame.srcNode, frame.seq);

  if ((frame.flags & kFlagAckRequired) != 0U) {
    sendBusAck(frame.srcNode, frame.seq, nowMs);
  }

  if ((frame.flags & kFlagFragmented) != 0U) {
    size_t assembledLength = 0;
    const FragmentAssembler::Status status =
        reassembler_.accept(frame, nowMs, appPayloadBuffer_, sizeof(appPayloadBuffer_),
                            assembledLength);
    if (status == FragmentAssembler::Status::kComplete) {
      // Zero copy delivery: the reassembled payload is handed to the application
      // as a view. Copying it into a Frame (480 bytes) overflowed the stack of the
      // loop task by up to 5232 bytes for a 12 fragment message.
      LOG_I("bus", "reassembled %s seq=%u from node %u (%u bytes, %u/%u fragments)",
            messageTypeName(frame.type), static_cast<unsigned>(frame.seq),
            static_cast<unsigned>(frame.srcNode),
            static_cast<unsigned>(reassembler_.lastLength()),
            static_cast<unsigned>(frame.payload[0]) + 1U,
            static_cast<unsigned>(frame.payload[1]));
      deliverToApp(frame.type, reassembler_.lastPayload(), reassembler_.lastLength());
    } else if (status == FragmentAssembler::Status::kError) {
      ++fragmentsRejected_;
      LOG_W("bus", "fragment rejected (%s seq=%u frag=%u/%u from node %u)", messageTypeName(frame.type),
            static_cast<unsigned>(frame.seq), static_cast<unsigned>(frame.payload[0]) + 1U,
            static_cast<unsigned>(frame.payload[1]), static_cast<unsigned>(frame.srcNode));
    }
    return;
  }

  deliverToApp(frame.type, frame.payload, frame.payloadLength);
}

void MeshNode::deliverToApp(uint8_t type, const uint8_t* payload, size_t length) {
  if (appHandler_ == nullptr) {
    return;
  }
  AppEnvelope envelope;
  if (!parseAppEnvelope(payload, length, envelope)) {
    LOG_W("app", "malformed application envelope (%u bytes)", static_cast<unsigned>(length));
    return;
  }
  LOG_D("app", "%s from %s to %s (%u bytes)", messageTypeName(type), envelope.source,
        envelope.destination, static_cast<unsigned>(envelope.bodyLength));
  appHandler_(context_, type, payload, length);
}

void MeshNode::sendBusAck(uint8_t destination, uint16_t ackedSeq, uint32_t nowMs) {
  const uint8_t payload[2] = {static_cast<uint8_t>(ackedSeq & 0xFFU),
                              static_cast<uint8_t>(ackedSeq >> 8)};
  const Frame ack = makeFrame(kTypeBusAck, destination, payload, sizeof(payload), 0);
  // A bus acknowledgement uses the dedicated control capacity: it can never take
  // the place of a user message, and it is never lost because the application
  // queue happened to be full.
  if (!queue_.pushProtected(FrameQueue::kPrioReceipt, ack, nowMs)) {
    ++controlQueueDrops_;
    LOG_W("bus", "control queue full, ack for seq=%u not queued", static_cast<unsigned>(ackedSeq));
  }
}

void MeshNode::passToken(uint8_t destination, uint8_t hops, uint32_t nowMs) {
  const uint8_t payload[2] = {destination, hops};
  const Frame token = makeFrame(kTypeToken, destination, payload, sizeof(payload), 0);
  if (bus_.send(token, nowMs)) {
    ++tokenPasses_;
    arbiter_.giveTokenAway(nowMs);
    tokenFromNode_ = 0;
    LOG_D("bus", "token passed to node %u (idle passes=%u)", static_cast<unsigned>(destination),
          static_cast<unsigned>(hops));
  } else {
    // The transceiver is still busy: the token is kept and retried next loop.
    LOG_D("bus", "token to node %u postponed (transceiver busy)",
          static_cast<unsigned>(destination));
  }
}

// The token holder has nothing to send: hand the token to the next alive station
// instead of dropping it, and only release the ring once every alive station had
// an empty turn. Dropping the token immediately (the previous behaviour) forced
// every new message to wait for a full claim timeout.
void MeshNode::serviceIdleToken(uint32_t nowMs) {
  const uint8_t next = arbiter_.nextAliveNode(config_->nodeId);
  if (next == config_->nodeId) {
    return;  // alone on the bus: keep the token, there is nobody to hand it to
  }
  if (static_cast<uint8_t>(arbiter_.idlePasses() + 1U) >= arbiter_.aliveCount()) {
    arbiter_.giveTokenAway(nowMs);
    LOG_D("bus", "token released after a full idle round");
    return;
  }
  arbiter_.noteIdlePass();
  passToken(next, arbiter_.idlePasses(), nowMs);
}

void MeshNode::serviceArbitration(uint32_t nowMs) {
  if (bus_.isTransmitting()) {
    return;
  }

  if (arbiter_.hasToken()) {
    if (!queue_.empty()) {
      if (arbiter_.mayTransmit(nowMs)) {
        Frame frame;
        if (!queue_.pop(frame, nowMs)) {
          return;
        }
        const bool unicast = frame.dstNode != kNodeBroadcast;
        const bool moreInTurn =
            !queue_.empty() && (arbiter_.framesThisTurn() + 1U < arbiter_.maxFramesPerTurn());
        if (!moreInTurn && unicast) {
          frame.flags = static_cast<uint8_t>(frame.flags | kFlagTokenPass);
        }
        if (!bus_.send(frame, nowMs)) {
          // Could not start the transfer: keep the frame for the next loop. The
          // re-queue is protected so a control frame is never lost here.
          if (!queue_.pushProtected(FrameQueue::kPrioControl, frame, nowMs)) {
            ++queueDrops_;
            LOG_W("bus", "cannot re-queue frame seq=%u after a send failure",
                  static_cast<unsigned>(frame.seq));
          }
          return;
        }
        arbiter_.noteDataFrame(nowMs);
        if ((frame.flags & kFlagAckRequired) != 0U &&
            (frame.flags & kFlagRetransmit) == 0U) {
          // Fragments of the same message share one entry: the acknowledgement
          // names the fragment, so a lost fragment is retransmitted alone.
          const uint16_t messageSeq =
              (frame.flags & kFlagFragmented) != 0U
                  ? static_cast<uint16_t>(frame.payload[2]) |
                        (static_cast<uint16_t>(frame.payload[3]) << 8)
                  : frame.seq;
          if (!ackTracker_.track(frame, messageSeq, nowMs, kAckTimeoutMs)) {
            LOG_W("bus", "ack tracker full, seq=%u not tracked (application level recovery)",
                  static_cast<unsigned>(frame.seq));
          }
        }
        if ((frame.flags & kFlagTokenPass) != 0U) {
          arbiter_.giveTokenAway(nowMs);
          tokenFromNode_ = 0;
        }
        return;
      }
      // Turn budget exhausted while work remains: give the next station a slot.
      const uint8_t next = arbiter_.nextAliveNode(config_->nodeId);
      if (next == config_->nodeId) {
        arbiter_.takeToken(nowMs, 0);  // alone on the bus, restart our turn
      } else {
        passToken(next, 0, nowMs);
      }
      return;
    }

    serviceIdleToken(nowMs);
    return;
  }

  if (!queue_.empty() && arbiter_.shouldClaim(nowMs)) {
    arbiter_.claim(nowMs);
    ++tokensClaimed_;
    tokenFromNode_ = 0;
    LOG_D("bus", "claimed token after %u ms of silence",
          static_cast<unsigned>(arbiter_.tokenTimeoutMs()));
  }
}

void MeshNode::servicePresence(uint32_t nowMs) {
  if (static_cast<uint32_t>(nowMs - lastPingMs_) >= kPingIntervalMs) {
    lastPingMs_ = nowMs;
    const uint8_t payload[1] = {config_->nodeId};
    const Frame ping = makeFrame(kTypePing, kNodeBroadcast, payload, sizeof(payload), 0);
    if (!queue_.push(FrameQueue::kPrioControl, ping, nowMs)) {
      LOG_W("pres", "queue full, ping not queued");
    }
  }

  if (presenceDirty_ ||
      static_cast<uint32_t>(nowMs - lastRouteStateMs_) >= kRouteStateIntervalMs) {
    broadcastRouteState(nowMs, FrameQueue::kPrioControl);
    presenceDirty_ = false;
    lastRouteStateMs_ = nowMs;
  }

  if (static_cast<uint32_t>(nowMs - lastAliveScanMs_) >= kAliveScanIntervalMs) {
    lastAliveScanMs_ = nowMs;
    arbiter_.setAliveMask(computeAliveMask(nowMs));

    // A phone that stopped refreshing its presence (Wi-Fi lost, app killed)
    // must not stay "online" forever in the routing table of the other cells.
    const size_t before = routing_.countOnline(config_->nodeId);
    routing_.expireLocal(config_->nodeId, nowMs, kLocalUserTimeoutMs);
    const size_t after = routing_.countOnline(config_->nodeId);
    if (after != before) {
      ++presenceExpirations_;
      presenceDirty_ = true;
      if (presenceHandler_ != nullptr) {
        presenceHandler_(context_);
      }
      LOG_I("pres", "local user presence expired: %u -> %u online",
            static_cast<unsigned>(before), static_cast<unsigned>(after));
    }
  }
}

void MeshNode::servicePeerLiveness(uint32_t nowMs) {
  for (uint8_t node = 1; node <= kMaxStationNodes; ++node) {
    if (node == config_->nodeId || !peerSeen_[node]) {
      continue;
    }
    if (static_cast<uint32_t>(nowMs - peerLastSeenMs_[node]) > kPeerTimeoutMs) {
      peerSeen_[node] = false;
      routing_.markStationOffline(node);
      presenceDirty_ = true;
      if (presenceHandler_ != nullptr) {
        presenceHandler_(context_);
      }
      LOG_W("pres", "node %u silent for %lu ms, its users are offline",
            static_cast<unsigned>(node), static_cast<unsigned long>(kPeerTimeoutMs));
    }
  }
}

void MeshNode::serviceRetransmissions(uint32_t nowMs) {
  // Selective retransmission: only the fragments that are still unacknowledged
  // are sent again, and only after a real bus silence (a frame received from the
  // peer after our send postpones the deadline instead of duplicating traffic).
  for (uint8_t guard = 0; guard <= kMaxAckMessages; ++guard) {
    Frame retry;
    const AckTracker::Action action = ackTracker_.poll(nowMs, lastRxMs_, retry);
    if (action == AckTracker::Action::kIdle) {
      return;
    }
    if (action == AckTracker::Action::kFailed) {
      LOG_W("bus", "no bus ack after %u attempts, giving up on seq=%u (application retry)",
            static_cast<unsigned>(kMaxAckAttempts), static_cast<unsigned>(retry.seq));
      continue;
    }
    if (!queue_.pushProtected(FrameQueue::kPrioControl, retry, nowMs)) {
      ++controlQueueDrops_;
      LOG_W("bus", "control queue full, retransmission seq=%u postponed",
            static_cast<unsigned>(retry.seq));
      return;
    }
    LOG_D("bus", "retransmitting %s seq=%u to node %u", messageTypeName(retry.type),
          static_cast<unsigned>(retry.seq), static_cast<unsigned>(retry.dstNode));
  }
}

bool MeshNode::isDuplicate(uint8_t srcNode, uint16_t seq) const {
  for (uint8_t i = 0; i < kDuplicateHistory; ++i) {
    if (duplicateNodes_[i] == srcNode && duplicateSeqs_[i] == seq) {
      return true;
    }
  }
  return false;
}

void MeshNode::rememberSequence(uint8_t srcNode, uint16_t seq) {
  duplicateNodes_[duplicateIndex_] = srcNode;
  duplicateSeqs_[duplicateIndex_] = seq;
  duplicateIndex_ = static_cast<uint8_t>((duplicateIndex_ + 1U) % kDuplicateHistory);
}

bool MeshNode::markPeerSeen(uint8_t node, uint32_t nowMs) {
  if (node == 0U || node > kMaxStationNodes) {
    return false;
  }
  const bool isNew = !peerSeen_[node];
  peerSeen_[node] = true;
  peerLastSeenMs_[node] = nowMs;
  return isNew;
}

uint8_t MeshNode::computeAliveMask(uint32_t nowMs) const {
  uint8_t mask = 0;
  for (uint8_t node = 1; node <= kMaxStationNodes; ++node) {
    bool alive = (node == config_->nodeId);
    if (!alive && peerSeen_[node] &&
        static_cast<uint32_t>(nowMs - peerLastSeenMs_[node]) <= kPeerTimeoutMs) {
      alive = true;
    }
    if (alive) {
      mask = static_cast<uint8_t>(mask | (1U << (node - 1U)));
    }
  }
  return mask;
}

void MeshNode::broadcastRouteState(uint32_t nowMs, FrameQueue::Priority priority) {
  uint8_t payload[kMaxBusPayload] = {0};
  const size_t length =
      routing_.serializeStationState(config_->nodeId, payload, sizeof(payload));
  if (length == 0U) {
    return;
  }
  const Frame state = makeFrame(kTypeRouteState, kNodeBroadcast, payload, length, 0);
  if (!queue_.push(priority, state, nowMs)) {
    LOG_W("pres", "queue full, route state not queued");
  }
}

bool MeshNode::registerLocalUser(const char* userId, const char* displayName) {
  if (!routing_.upsert(userId, displayName, config_->nodeId, millis())) {
    LOG_W("app", "cannot register local user (table full?)");
    return false;
  }
  presenceDirty_ = true;
  return true;
}

bool MeshNode::refreshLocalUser(const char* userId) {
  const bool touched = routing_.touch(userId, millis());
  if (!touched) {
    presenceDirty_ = true;
  }
  return touched;
}

void MeshNode::removeLocalUser(const char* userId) {
  if (routing_.remove(userId)) {
    presenceDirty_ = true;
  }
}

void MeshNode::publishPresence() {
  presenceDirty_ = true;
}

}  // namespace cell
