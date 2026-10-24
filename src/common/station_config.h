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
constexpr size_t kUartBufferSize = 1024;

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
};

const StationConfig& stationConfig();

}  // namespace cell

#endif  // STATION_CONFIG_H
