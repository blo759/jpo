#ifndef CELL_APP_SERVER_H
#define CELL_APP_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include "cell_core.h"
#include "mesh_node.h"
#include "station_config.h"

namespace cell {

// Local application service exposed by each station:
//   * HTTP REST on servicePort  : identity, presence, text, voice blocks
//   * WebSocket on wsPort       : real time events (incoming message, receipts,
//                                 voice blocks, presence updates)
class AppServer {
 public:
  AppServer(MeshNode& mesh, const StationConfig& config);

  void begin();
  void loop();

  // Called by the mesh node when a bus frame is addressed to this station. The
  // payload is a view: for a reassembled message it can be larger than one bus
  // frame, so it is passed with its length instead of a Frame.
  void onAppFrame(uint8_t type, const uint8_t* payload, size_t length);
  void onPresenceChanged();

 private:
  void handleInfo();
  void handleHealth();
  void handleUsers();
  void handlePresenceRegister();
  void handlePresenceLeave();
  void handleText();
  void handleReceipt();
  void handleVoiceBlock();
  void handleVoiceFeedback();
  void handleNotFound();

  void handleSocketEvent(uint8_t clientNumber, uint8_t type, uint8_t* payload, size_t length);
  void bindSocketToUser(uint8_t clientNumber, const char* userId);
  void sendJson(int code, const String& body);
  void sendStatus(int code, const char* status, const char* detail = nullptr);
  void replyRouteStatus(MeshNode::RouteStatus status, const char* targetUser);
  void pushToUser(const char* userId, const String& json);
  // The binary voice frame carries its own identity (msgId, seq, total) so a lost
  // or reordered JSON header can never associate the audio bytes with the wrong
  // block. See kVoiceBinaryMagic.
  void pushVoiceBlockToUser(const char* userId,
                            const char* msgId,
                            uint16_t seq,
                            uint16_t total,
                            const uint8_t* data,
                            size_t length);
  void pushPresence();
  size_t buildUserListJson(String& out) const;

  MeshNode& mesh_;
  const StationConfig& config_;
  WebServer http_;
  WebSocketsServer ws_;
  char socketUsers_[WEBSOCKETS_SERVER_CLIENT_MAX][kMaxUserIdLength];
  uint32_t socketEvents_ = 0;
  uint32_t pushedEvents_ = 0;
};

}  // namespace cell

#endif  // CELL_APP_SERVER_H
