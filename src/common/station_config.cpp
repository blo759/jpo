#include "station_config.h"

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

namespace cell {
namespace {

const char* const kFirmwareVersion = "1.0.0";

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
};

}  // namespace

const StationConfig& stationConfig() {
  return kStationConfig;
}

}  // namespace cell
