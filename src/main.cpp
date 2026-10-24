#include <Arduino.h>
#include <WiFi.h>

#include "app_server.h"
#include "led_indicators.h"
#include "log.h"
#include "mesh_node.h"
#include "station_config.h"
#include "station_link.h"
#include "wifi_backbone.h"

namespace {

cell::LedIndicators gLeds;
cell::MeshNode gMesh(Serial2, gLeds);
cell::WifiBackbone gWifi;
cell::StationLink gLink(gWifi);
cell::AppServer gServer(gMesh, gLink, cell::stationConfig());

void onAppFrame(void* /*context*/, uint8_t type, const uint8_t* payload, size_t length) {
  gServer.onAppFrame(type, payload, length);
}

void onPresenceChanged(void* /*context*/) {
  gServer.onPresenceChanged();
}

// Wi-Fi backbone callbacks. The link authenticates the stations and enforces the
// hop limit; once a frame is accepted it is handed to the application server,
// which treats it exactly like a local call from there on.
void onLinkSignal(void* /*context*/, const cell::LinkCallSignal& signal, uint8_t fromNode) {
  gServer.onLinkSignal(signal, fromNode);
}

void onLinkAudio(void* /*context*/, uint8_t* audioFrame, size_t length, uint8_t fromNode) {
  gServer.onLinkAudio(audioFrame, length, fromNode);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);  // let the USB-UART settle before the first log line

  cell::setLogLevel(cell::kLogInfo);
  const cell::StationConfig& config = cell::stationConfig();
  LOG_I("boot", "local cellular prototype - %s - firmware %s", config.siteId,
        config.firmware);

  // Access point (unchanged) plus, for the stations that are not the hub, the
  // station interface that attaches to the upstream access point.
  gWifi.begin(config);
  gMesh.begin(config, onAppFrame, onPresenceChanged, nullptr);
  gLink.begin(config, onLinkSignal, onLinkAudio, nullptr);
  gServer.begin();

  if (cell::stationUsesDemoLinkToken()) {
    LOG_W("boot",
          "the inter-station link token is still the DEMONSTRATION value from "
          "platformio.ini: change it before any real deployment");
  }

  LOG_I("boot", "ready: http://%s:%u/api/info (calls: local + cross cell over the "
                "Wi-Fi backbone)",
        config.apIp, static_cast<unsigned>(config.servicePort));
}

void loop() {
  const uint32_t now = millis();
  gServer.loop();
  gMesh.loop();
  // The radio is serviced before the link so a lost association is noticed
  // before the TCP sockets are inspected.
  gWifi.loop(now);
  gLink.loop(now);
  delay(1);
}
