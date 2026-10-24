#include "station_config.h"

#include <cstring>

#ifndef SITE_NODE_ID
#define SITE_NODE_ID 1
#endif

#ifndef SITE_ID
#define SITE_ID "SITE_A"
#endif

#ifndef SITE_NAME
#define SITE_NAME "CELL_SITE_A"
#endif

#ifndef APP_WIFI_SSID
#define APP_WIFI_SSID "CELL_SITE_A"
#endif

#ifndef APP_WIFI_PASSWORD
#define APP_WIFI_PASSWORD "cellsitea"
#endif

#ifndef APP_AP_IP
#define APP_AP_IP "10.0.0.1"
#endif

#ifndef APP_SERVICE_PORT
#define APP_SERVICE_PORT 8080
#endif

#ifndef APP_WS_PORT
#define APP_WS_PORT 8081
#endif

#ifndef APP_RS485_TERMINATED
#define APP_RS485_TERMINATED 0
#endif

// ---------------------------------------------------------------------------
// Wi-Fi backbone defaults (overridden per environment in platformio.ini).
// ---------------------------------------------------------------------------
#ifndef APP_WIFI_CHANNEL
#define APP_WIFI_CHANNEL 1
#endif

#ifndef APP_LINK_PORT
#define APP_LINK_PORT 8082
#endif

#ifndef APP_LINK_UPSTREAM_NODE
#define APP_LINK_UPSTREAM_NODE 0
#endif

#ifndef APP_LINK_UPSTREAM_SSID
#define APP_LINK_UPSTREAM_SSID ""
#endif

#ifndef APP_LINK_UPSTREAM_PASSWORD
#define APP_LINK_UPSTREAM_PASSWORD ""
#endif

#ifndef APP_LINK_UPSTREAM_IP
#define APP_LINK_UPSTREAM_IP "10.0.0.1"
#endif

// DEMONSTRATION VALUE. Replace it (per environment, or through your own build
// flags) before any real deployment; a token still equal to this value is
// reported as a warning at boot.
#ifndef APP_LINK_TOKEN
#define APP_LINK_TOKEN "cellnet-demo-link-token"
#endif

#ifndef APP_LINK_RETRY_MS
#define APP_LINK_RETRY_MS 2000
#endif

namespace cell {
namespace {

const char* const kFirmwareVersion = "1.1.0";

// Reported at boot when the shared link token is still the demonstration value.
const char* const kDemoLinkToken = "cellnet-demo-link-token";

const StationConfig kStationConfig = {
    static_cast<uint8_t>(SITE_NODE_ID),
    SITE_ID,
    SITE_NAME,
    APP_WIFI_SSID,
    APP_WIFI_PASSWORD,
    APP_AP_IP,
    static_cast<uint16_t>(APP_SERVICE_PORT),
    static_cast<uint16_t>(APP_WS_PORT),
    (APP_RS485_TERMINATED != 0),
    kFirmwareVersion,
    static_cast<uint8_t>(APP_WIFI_CHANNEL),
    static_cast<uint16_t>(APP_LINK_PORT),
    static_cast<uint8_t>(APP_LINK_UPSTREAM_NODE),
    APP_LINK_UPSTREAM_SSID,
    APP_LINK_UPSTREAM_PASSWORD,
    APP_LINK_UPSTREAM_IP,
    APP_LINK_TOKEN,
    static_cast<uint32_t>(APP_LINK_RETRY_MS),
};

}  // namespace

const StationConfig& stationConfig() {
  return kStationConfig;
}

bool stationUsesDemoLinkToken() {
  const char* token = kStationConfig.linkToken;
  if (token == nullptr) {
    return true;
  }
  return strcmp(token, kDemoLinkToken) == 0;
}

}  // namespace cell
