#ifndef STATION_CONFIG_H
#define STATION_CONFIG_H

#include <Arduino.h>

namespace cell {

// ---------------------------------------------------------------------------
// Wiring imposed by the specification: do not change these pins.
// ---------------------------------------------------------------------------
constexpr uint8_t kRs485TxPin = 17;   // GPIO17 / TX2 -> DI
constexpr uint8_t kRs485RxPin = 16;   // GPIO16 / RX2 -> RO
constexpr uint8_t kRs485DirPin = 4;   // GPIO4       -> DE + RE tied together
constexpr uint8_t kLedTxPin = 25;     // green LED   -> real RS485 emission
constexpr uint8_t kLedRxPin = 26;     // red LED     -> real RS485 reception

constexpr uint8_t kRs485UartPort = 2;
constexpr uint32_t kRs485Baud = 115200;
// RS485 UART buffers. The receive buffer is what absorbs a main loop that is
// momentarily busy with Wi-Fi, HTTP, WebSocket or the inter-station link: at
// 115200 baud 4096 bytes hold 355 ms of bus traffic instead of 89 ms, so a
// transient stall can no longer make the station drop bus frames, that is text
// messages, recorded voice blocks and their acknowledgements.
constexpr size_t kUartRxBufferSize = 4096;
constexpr size_t kUartTxBufferSize = 2048;

// Inter-station IP link (Wi-Fi backbone). 8080 (HTTP) and 8081 (WebSocket) are
// unchanged; the backbone has its own port.
constexpr uint16_t kLinkDefaultPort = 8082;
constexpr uint32_t kLinkDefaultRetryMs = 2000;
constexpr uint8_t kLinkDefaultChannel = 1;

struct StationConfig {
  uint8_t nodeId;          // 1..3, stable identity of the station on the bus
  const char* siteId;      // "SITE_A"
  const char* siteName;    // "CELL_SITE_A"
  const char* wifiSsid;    // SSID of the local access point
  const char* wifiPassword;
  const char* apIp;        // static AP address, e.g. "10.0.0.1"
  uint16_t servicePort;    // HTTP REST API
  uint16_t wsPort;         // WebSocket event channel
  bool rs485Terminated;    // documentation: 120 ohm fitted on this station
  const char* firmware;

  // ---------------------------------------------------------------------
  // Wi-Fi backbone (inter-station IP link), see README section 18
  // ---------------------------------------------------------------------
  // An ESP32 has one radio: the access point and the station interface share it
  // and MUST use the same channel. All three stations advertise their AP on it.
  uint8_t wifiChannel;
  uint16_t linkPort;        // TCP port of the inter-station link
  uint8_t upstreamNode;     // station this one attaches to; 0 = hub
  const char* upstreamSsid; // SSID of the upstream access point ("" when hub)
  const char* upstreamPassword;
  const char* upstreamIp;   // address of the upstream access point
  const char* linkToken;    // shared secret authenticating the stations
  uint32_t linkRetryMs;     // reconnection delay of the station interface
};

const StationConfig& stationConfig();

// True when the shared inter-station token is still the demonstration value
// committed in platformio.ini. Used to warn at boot, never to refuse to start.
bool stationUsesDemoLinkToken();

}  // namespace cell

#endif  // STATION_CONFIG_H
