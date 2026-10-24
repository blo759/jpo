#ifndef CELL_STATION_LINK_H
#define CELL_STATION_LINK_H

#include <Arduino.h>
#include <WiFi.h>

#include "cell_link.h"
#include "station_config.h"
#include "wifi_backbone.h"

namespace cell {

// Inter-station IP link: the Wi-Fi backbone that carries cross cell call
// signalling and call audio.
//
//   * TCP server: accepts the connections of the stations attached to this one
//     (the hub of the star accepts two);
//   * TCP client: opens and maintains the connection toward the upstream station
//     (nothing for the hub);
//   * framing: the binary protocol of lib/cell_link, authenticated by a shared
//     token;
//   * relaying: a frame addressed to a third station is forwarded with a hop
//     counter and never back to the station it came from.
//
// Deliberately independent from the mesh, the bus arbitration and the bus frame
// queues: the backbone is not the RS485 bus and shares nothing with it. All the
// call audio goes through here or through the local WebSocket channel, never
// through the bus.
class StationLink {
 public:
  using SignalHandler = void (*)(void* context, const LinkCallSignal& signal, uint8_t fromNode);
  using AudioHandler =
      void (*)(void* context, uint8_t* audioFrame, size_t length, uint8_t fromNode);

  explicit StationLink(WifiBackbone& wifi);

  void begin(const StationConfig& config,
             SignalHandler signalHandler,
             AudioHandler audioHandler,
             void* context);
  void loop(uint32_t nowMs);

  /** Sends a signalling message to the station hosting `dstNode`. */
  bool sendSignal(const LinkCallSignal& signal, uint8_t dstNode);
  /** Relays one encoded call media frame (buildCallAudioFrame output). */
  bool sendAudio(const uint8_t* audioFrame, size_t length, uint8_t dstNode);

  bool reachable(uint8_t node) const;
  bool upstreamReady() const;
  uint8_t directMask() const { return routes_.directMask(); }
  bool hasPeer(uint8_t node) const;

  uint32_t framesSent() const { return framesSent_; }
  uint32_t framesReceived() const { return framesReceived_; }
  uint32_t framesRelayed() const { return framesRelayed_; }
  uint32_t framesDropped() const { return framesDropped_; }
  uint32_t signalsSent() const { return signalsSent_; }
  uint32_t signalsReceived() const { return signalsReceived_; }
  uint32_t audioSent() const { return audioSent_; }
  uint32_t audioReceived() const { return audioReceived_; }
  uint32_t authRejections() const { return authRejections_; }
  uint32_t disconnects() const { return disconnects_; }
  // Bytes still waiting in the outbound buffers of every socket, and the worst
  // time spent inside loop() since boot. Both are what tells a backbone problem
  // apart from a bus problem in /api/health.
  size_t txPending() const;
  uint32_t maxLoopUs() const { return maxLoopUs_; }
  const LinkRoutes& routes() const { return routes_; }
  const LinkClientState& upstream() const { return upstream_; }

  void logStatus(const char* reason) const;

 private:
  // The last slot is reserved for the outgoing (upstream) connection.
  static constexpr size_t kSocketSlots = kLinkMaxPeers + 1;

  struct Socket {
    WiFiClient client;
    LinkStreamAssembler assembler;
    LinkLiveness liveness;
    // Outbound buffers, one ring per traffic class: a burst of call audio can
    // therefore never push out a call signal. They are drained with at most one
    // non blocking write each per loop, so a congested socket never stalls the
    // main loop (and never delays the RS485 bus).
    uint8_t controlStorage[kLinkTxControlSize] = {0};
    uint8_t audioStorage[kLinkTxBufferSize] = {0};
    LinkTxBuffer control;
    LinkTxBuffer audio;
    uint8_t node = 0;
    bool authenticated = false;
    bool outgoing = false;
    bool active = false;

    Socket()
        : control(controlStorage, sizeof(controlStorage)),
          audio(audioStorage, sizeof(audioStorage)) {}
    // The buffers point into their own storage: a copied socket would write to
    // the storage of another one.
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
  };

  Socket* slotForNode(uint8_t node);
  Socket* freeSlot();
  Socket& outgoingSlot();
  void closeSlot(Socket& slot, const char* reason);
  void acceptConnections(uint32_t nowMs);
  void serviceUpstream(uint32_t nowMs);
  void serviceSocket(Socket& slot, uint32_t nowMs);
  void handleFrame(Socket& slot, LinkFrame& frame, uint32_t nowMs);
  void relayFrame(Socket& incoming, LinkFrame& frame);
  // Queues one encoded frame. `control` selects the ring that must never be
  // starved by call audio.
  bool enqueueFrame(Socket& slot, LinkFrame& frame, bool control);
  void flushSocket(Socket& slot);
  static size_t writeSocket(void* context, const uint8_t* data, size_t length);
  void sendHeartbeats(uint32_t nowMs);
  void sendRouteState(Socket& slot);

  WifiBackbone& wifi_;
  const StationConfig* config_ = nullptr;
  SignalHandler signalHandler_ = nullptr;
  AudioHandler audioHandler_ = nullptr;
  void* context_ = nullptr;

  WiFiServer server_;
  LinkRoutes routes_;
  LinkClientState upstream_;
  Socket sockets_[kSocketSlots];

  uint32_t lastHeartbeatMs_ = 0;
  uint32_t lastRouteStateMs_ = 0;
  // Progressive reconnection backoff: a station that keeps failing must stop
  // hammering the radio, and the connect() call is synchronous.
  uint32_t connectDelayMs_ = 0;
  uint32_t maxLoopUs_ = 0;
  uint32_t framesSent_ = 0;
  uint32_t framesReceived_ = 0;
  uint32_t framesRelayed_ = 0;
  uint32_t framesDropped_ = 0;
  uint32_t signalsSent_ = 0;
  uint32_t signalsReceived_ = 0;
  uint32_t audioSent_ = 0;
  uint32_t audioReceived_ = 0;
  uint32_t authRejections_ = 0;
  uint32_t disconnects_ = 0;
};

}  // namespace cell

#endif  // CELL_STATION_LINK_H
