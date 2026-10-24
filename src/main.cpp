#include <Arduino.h>
#include <WiFi.h>

#include "app_server.h"
#include "led_indicators.h"
#include "log.h"
#include "mesh_node.h"
#include "station_config.h"

namespace {

cell::LedIndicators gLeds;
cell::MeshNode gMesh(Serial2, gLeds);
cell::AppServer gServer(gMesh, cell::stationConfig());

void onAppFrame(void* /*context*/, uint8_t type, const uint8_t* payload, size_t length) {
  gServer.onAppFrame(type, payload, length);
}

void onPresenceChanged(void* /*context*/) {
  gServer.onPresenceChanged();
}

void startAccessPoint() {
  const cell::StationConfig& config = cell::stationConfig();
  WiFi.mode(WIFI_AP);

  IPAddress address;
  if (!address.fromString(config.apIp)) {
    address = IPAddress(10, 0, 0, 1);
  }
  const IPAddress gateway = address;
  const IPAddress mask(255, 255, 255, 0);
  if (!WiFi.softAPConfig(address, gateway, mask)) {
    LOG_W("wifi", "softAPConfig failed, default AP address will be used");
  }

  const bool started = WiFi.softAP(config.wifiSsid, config.wifiPassword, 1, false, 6);
  if (started) {
    LOG_I("wifi", "access point \"%s\" up, IP %s", config.wifiSsid,
          WiFi.softAPIP().toString().c_str());
  } else {
    LOG_E("wifi", "access point \"%s\" failed to start", config.wifiSsid);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);  // let the USB-UART settle before the first log line

  cell::setLogLevel(cell::kLogInfo);
  const cell::StationConfig& config = cell::stationConfig();
  LOG_I("boot", "local cellular prototype - %s - firmware %s", config.siteId,
        config.firmware);

  startAccessPoint();
  gMesh.begin(config, onAppFrame, onPresenceChanged, nullptr);
  gServer.begin();

  LOG_I("boot", "ready: http://%s:%u/api/info", config.apIp,
        static_cast<unsigned>(config.servicePort));
}

void loop() {
  gServer.loop();
  gMesh.loop();
  delay(1);
}
