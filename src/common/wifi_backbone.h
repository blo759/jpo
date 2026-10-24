#ifndef CELL_WIFI_BACKBONE_H
#define CELL_WIFI_BACKBONE_H

#include <Arduino.h>
#include <WiFi.h>

#include "station_config.h"

namespace cell {

// Wi-Fi of a station:
//   * the access point for the phones is kept exactly as before (SSID, password,
//     static address, client count);
//   * when the station is not the hub of the backbone, a station interface is
//     added on the same radio and attached to the upstream access point.
//
// An ESP32 has a single radio, so the access point and the station interface
// must share the channel: the AP channel comes from the configuration and is the
// same on the three stations. If the association succeeds on another channel,
// the access point would be disrupted, so the mismatch is reported.
//
// This class only manages the radio. It never touches the RS485 bus, the mesh
// node or the call table.
class WifiBackbone {
 public:
  struct Status {
    bool apUp = false;
    bool staConfigured = false;
    bool staConnected = false;
    uint8_t channel = 0;
    uint8_t upstreamNode = 0;
    IPAddress apIp;
    IPAddress staIp;
    uint32_t reconnects = 0;
    int32_t rssi = 0;
  };

  void begin(const StationConfig& config);
  // Keeps the station interface attached: reconnects it when it drops.
  void loop(uint32_t nowMs);
  Status status() const;
  // True when the backbone can be used from this station (hub, or attached).
  bool upstreamReady() const;

 private:
  void logAttach(uint32_t nowMs);

  const StationConfig* config_ = nullptr;
  bool staConfigured_ = false;
  bool staConnected_ = false;
  uint32_t lastAttemptMs_ = 0;
  uint32_t lastLogMs_ = 0;
  uint32_t reconnects_ = 0;
  uint32_t nextAttemptMs_ = 0;
};

}  // namespace cell

#endif  // CELL_WIFI_BACKBONE_H
