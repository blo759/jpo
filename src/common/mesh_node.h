#ifndef CELL_MESH_NODE_H
#define CELL_MESH_NODE_H

#include <Arduino.h>

#include "cell_core.h"
#include "cell_protocol.h"
#include "led_indicators.h"
#include "rs485_bus.h"
#include "station_config.h"

namespace cell {

// Glue between the RS485 transport and the local Wi-Fi application service.
// Responsibilities: token arbitration, presence/routing, bus level
// acknowledgement, fragmentation, local delivery.
class MeshNode {
 public:
  // The application handler receives a payload view (type + bytes + length)
  // instead of a Frame. A reassembled message can be up to
  // kMaxReassembledPayload bytes, far more than Frame::payload can hold: passing
  // the view is what removed a stack overflow of up to 5232 bytes.
  using AppFrameHandler = void (*)(void* context, uint8_t type, const uint8_t* payload,
                                   size_t length);
  using PresenceHandler = void (*)(void* context);

  enum class RouteStatus : uint8_t {
    kQueued,
    kDeliveredLocally,
    kUnknownRecipient,
    kRecipientOffline,
    kQueueFull,
    kTooLarge,
  };

  MeshNode(HardwareSerial& serial, LedIndicators& leds);

  void begin(const StationConfig& config,
             AppFrameHandler appHandler,
             PresenceHandler presenceHandler,
             void* context);
  void loop();

  RouteStatus submitAppPayload(uint8_t type,
                               const uint8_t* body,
                               size_t bodyLength,
                               const char* fromUser,
                               const char* toUser,
                               FrameQueue::Priority priority,
                               bool requireAck);

  bool registerLocalUser(const char* userId, const char* displayName);
  bool refreshLocalUser(const char* userId);
  void removeLocalUser(const char* userId);
  void publishPresence();

  const StationConfig& config() const { return *config_; }
  const RoutingTable& routing() const { return routing_; }
  const BusArbiter& arbiter() const { return arbiter_; }
  const Rs485Bus& bus() const { return bus_; }
  const FrameQueue& queue() const { return queue_; }
  const AckTracker& acks() const { return ackTracker_; }
  uint8_t selfNode() const { return config_->nodeId; }
  uint32_t uptimeSeconds() const { return (millis() - bootMs_) / 1000UL; }
  uint32_t maxLoopMicros() const { return maxLoopUs_; }
  uint32_t queueDrops() const { return queueDrops_; }
  uint32_t controlQueueDrops() const { return controlQueueDrops_; }
  uint32_t fragmentsRejected() const { return fragmentsRejected_; }
  uint32_t tokensClaimed() const { return tokensClaimed_; }

  uint32_t transportRetries() const { return ackTracker_.retransmissions(); }
  uint32_t transportFailures() const { return ackTracker_.failures(); }
  uint32_t ackTimeouts() const { return ackTracker_.timeouts(); }
  uint32_t duplicatesDropped() const { return duplicates_; }
  uint32_t tokensReceived() const { return tokensReceived_; }
  uint32_t tokenPasses() const { return tokenPasses_; }
  uint32_t presenceExpirations() const { return presenceExpirations_; }
  uint32_t reassembledMessages() const { return reassembler_.completedCount(); }

  // Serial console: single letter commands, used by scripts/serial_capture.ps1 to
  // read the live bus/token/routing state without any Wi-Fi or HTTP access.
  void pollSerialConsole();
  void logBusSummary(const char* reason);

 private:
  static constexpr uint8_t kDuplicateHistory = 8;

  void handleBusFrame(Frame& frame, uint32_t nowMs);
  void handleApplicationFrame(const Frame& frame, uint32_t nowMs);
  void deliverToApp(uint8_t type, const uint8_t* payload, size_t length);
  void sendBusAck(uint8_t destination, uint16_t ackedSeq, uint32_t nowMs);
  void passToken(uint8_t destination, uint8_t hops, uint32_t nowMs);
  void serviceIdleToken(uint32_t nowMs);

  void serviceArbitration(uint32_t nowMs);
  void servicePresence(uint32_t nowMs);
  void servicePeerLiveness(uint32_t nowMs);
  void serviceRetransmissions(uint32_t nowMs);

  bool isDuplicate(uint8_t srcNode, uint16_t seq) const;
  void rememberSequence(uint8_t srcNode, uint16_t seq);
  bool markPeerSeen(uint8_t node, uint32_t nowMs);
  uint8_t computeAliveMask(uint32_t nowMs) const;
  void broadcastRouteState(uint32_t nowMs, FrameQueue::Priority priority);
  Frame makeFrame(uint8_t type,
                  uint8_t destination,
                  const uint8_t* payload,
                  size_t length,
                  uint8_t flags);

  HardwareSerial& serial_;
  LedIndicators& leds_;
  Rs485Bus bus_;
  FrameQueue queue_;
  BusArbiter arbiter_;
  RoutingTable routing_;
  FragmentAssembler reassembler_;
  AckTracker ackTracker_;

  // Bounded, statically allocated working buffer used to build an outgoing
  // application envelope. Incoming reassembled messages are delivered as a zero
  // copy view of the assembler context, so no second buffer is needed here (and
  // 5712 bytes of RAM are saved).
  uint8_t appPayloadBuffer_[kMaxReassembledPayload] = {0};

  const StationConfig* config_ = nullptr;
  AppFrameHandler appHandler_ = nullptr;
  PresenceHandler presenceHandler_ = nullptr;
  void* context_ = nullptr;

  uint32_t bootMs_ = 0;
  uint16_t nextSequence_ = 1;
  uint32_t lastPingMs_ = 0;
  uint32_t lastRouteStateMs_ = 0;
  uint32_t lastAliveScanMs_ = 0;
  uint32_t lastRxMs_ = 0;
  bool presenceDirty_ = true;
  uint8_t tokenFromNode_ = 0;

  uint32_t peerLastSeenMs_[kMaxStationNodes + 1] = {0};
  bool peerSeen_[kMaxStationNodes + 1] = {false};

  uint8_t duplicateNodes_[kDuplicateHistory] = {0};
  uint16_t duplicateSeqs_[kDuplicateHistory] = {0};
  uint8_t duplicateIndex_ = 0;

  uint32_t retries_ = 0;
  uint32_t failures_ = 0;
  uint32_t duplicates_ = 0;
  uint32_t tokensReceived_ = 0;
  uint32_t tokenPasses_ = 0;
  uint32_t queueDrops_ = 0;
  uint32_t controlQueueDrops_ = 0;
  uint32_t fragmentsRejected_ = 0;
  uint32_t tokensClaimed_ = 0;
  uint32_t maxLoopUs_ = 0;
  uint32_t presenceExpirations_ = 0;
  uint32_t lastSummaryMs_ = 0;
};

}  // namespace cell

#endif  // CELL_MESH_NODE_H
