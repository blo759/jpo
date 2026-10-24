#ifndef CELL_APP_SERVER_H
#define CELL_APP_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include "cell_call.h"
#include "cell_core.h"
#include "cell_link.h"
#include "mesh_node.h"
#include "station_config.h"
#include "station_link.h"

namespace cell {

// Local application service exposed by each station:
//   * HTTP REST on servicePort  : identity, presence, text, voice blocks, calls
//   * WebSocket on wsPort       : real time events (incoming message, receipts,
//                                 voice blocks, presence updates) and the binary
//                                 media channel of a live call
//
// Calls are local by default (both participants on this station, audio relayed
// between two local sockets) and cross cell through the Wi-Fi backbone when the
// other participant is on another station. In both cases the signalling and the
// audio are IP only: nothing is submitted to the mesh and no bus frame type was
// added for calls. See lib/cell_call and lib/cell_link.
class AppServer {
 public:
  AppServer(MeshNode& mesh, StationLink& link, const StationConfig& config);

  void begin();
  void loop();

  // Called by the mesh node when a bus frame is addressed to this station. The
  // payload is a view: for a reassembled message it can be larger than one bus
  // frame, so it is passed with its length instead of a Frame.
  void onAppFrame(uint8_t type, const uint8_t* payload, size_t length);
  void onPresenceChanged();

  // Called by the Wi-Fi backbone when the other station of a cross cell call
  // sends signalling or audio. Both are local to this station from there on.
  void onLinkSignal(const LinkCallSignal& signal, uint8_t fromNode);
  void onLinkAudio(uint8_t* audioFrame, size_t length, uint8_t fromNode);

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

  // Real time calls. Signalling over HTTP and the Wi-Fi backbone, media over the
  // binary WebSocket channel and the backbone. None of these methods touches the
  // mesh: a call never enters the RS485 egress queue.
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
  // Cross cell helpers.
  void handleCallInviteRemote(const char* callId, const char* from, const char* to,
                              const RouteEntry& target);
  bool sendRemoteSignal(const CallSession& session, uint8_t action, uint8_t reason = 0);
  const char* localParticipant(const CallSession& session) const;
  void releaseRemoteSession(const CallSession& session, uint8_t action, uint8_t reason);

  // Membership probe handed to the call table: a user is local when one of this
  // station's WebSocket clients is currently bound to that identifier. That is
  // the only proof that the user really is on this cell, and it is what makes a
  // cross cell call impossible even if the client lies about its cell.
  static bool probeLocalUser(void* context, const char* userId);
  bool hasLocalSocket(const char* userId) const;

  void handleSocketEvent(uint8_t clientNumber, uint8_t type, uint8_t* payload, size_t length);
  // Binds a socket to a user. The display name is optional and only used to
  // register the user on this station: a user that moves to another cell must
  // keep its announced name instead of falling back to its raw identifier.
  void bindSocketToUser(uint8_t clientNumber, const char* userId, const char* displayName);
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
  StationLink& link_;
  const StationConfig& config_;
  WebServer http_;
  WebSocketsServer ws_;
  char socketUsers_[WEBSOCKETS_SERVER_CLIENT_MAX][kMaxUserIdLength];
  uint32_t socketEvents_ = 0;
  uint32_t pushedEvents_ = 0;

  // Call sessions of this station. Bounded and statically allocated; it is never
  // shared with the mesh, so a call can never enter the RS485 bus.
  CallTable calls_;
  uint32_t callAudioFrames_ = 0;     // binary frames received from clients
  uint32_t callAudioRelayed_ = 0;    // frames really forwarded to the peer
  uint32_t callAudioRejected_ = 0;   // frames refused (bad format, no session)
  uint32_t callAudioSilent_ = 0;     // relays that found no peer socket
  // Cross cell counters (additive diagnostics).
  uint32_t callAudioForwarded_ = 0;   // frames sent to another cell over the backbone
  uint32_t callAudioFromLink_ = 0;    // frames received from another cell
  uint32_t callSignalsSent_ = 0;      // call signalling sent to another cell
  uint32_t callSignalsReceived_ = 0;  // call signalling received from another cell
  uint32_t callRemote_ = 0;           // cross cell sessions created
};

}  // namespace cell

#endif  // CELL_APP_SERVER_H
