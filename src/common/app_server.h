#ifndef CELL_APP_SERVER_H
#define CELL_APP_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include "cell_call.h"
#include "cell_core.h"
#include "mesh_node.h"
#include "station_config.h"

namespace cell {

// Local application service exposed by each station:
//   * HTTP REST on servicePort  : identity, presence, text, voice blocks, calls
//   * WebSocket on wsPort       : real time events (incoming message, receipts,
//                                 voice blocks, presence updates) and the binary
//                                 media channel of a live local call
//
// Real time calls are strictly local to the cell: the signalling never reaches
// the mesh and the audio is relayed as binary WebSocket frames between the two
// sockets of *this* station. See lib/cell_call for the session rules.
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

  // Real time calls, local cell only. Signalling over HTTP, media over the
  // binary WebSocket channel. None of these methods touches the mesh.
  void handleCallInvite();
  void handleCallAccept();
  void handleCallReject();
  void handleCallCancel();
  void handleCallHangup();
  void handleCallState();
  void handleCallAudio(uint8_t clientNumber, uint8_t* payload, size_t length);
  void serviceCallTimeouts();
  void notifyCallEnded(const char* userId, const char* callId, const char* peer,
                       const char* event, const char* reason);
  // Used when the participants are no longer known (an expired invitation): the
  // event carries the call id, every local client filters on it.
  void broadcastCallEvent(const char* event, const char* callId, const char* reason);
  void endCallsForUser(const char* userId, const char* event, const char* reason);
  void replyCallStatus(cell::CallStatus status);
  void pushCallEvent(const char* userId, const char* event, const CallSession& session,
                     const char* reason, const char* extraJson);

  // Membership probe handed to the call table: a user is local when one of this
  // station's WebSocket clients is currently bound to that identifier. That is
  // the only proof that the user really is on this cell, and it is what makes a
  // cross cell call impossible even if the client lies about its cell.
  static bool probeLocalUser(void* context, const char* userId);
  bool hasLocalSocket(const char* userId) const;

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

  // Call sessions of this cell. Bounded and statically allocated; it is never
  // shared with the mesh, so a call can never leave the cell.
  CallTable calls_;
  uint32_t callAudioFrames_ = 0;     // binary frames received from clients
  uint32_t callAudioRelayed_ = 0;    // frames really forwarded to the peer
  uint32_t callAudioRejected_ = 0;   // frames refused (bad format, no session)
  uint32_t callAudioSilent_ = 0;     // relays that found no peer socket
};

}  // namespace cell

#endif  // CELL_APP_SERVER_H
