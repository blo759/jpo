#include "wifi_backbone.h"

#include "log.h"

namespace cell {

void WifiBackbone::begin(const StationConfig& config) {
  config_ = &config;
  staConfigured_ = false;
  staConnected_ = false;
  reconnects_ = 0;

  const bool wantsBackbone = (config.upstreamNode != 0) && (config.upstreamSsid != nullptr) &&
                             (config.upstreamSsid[0] != '\0');

  // The access point is the original one; only the mode changes when the station
  // also needs a station interface.
  WiFi.mode(wantsBackbone ? WIFI_AP_STA : WIFI_AP);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);  // the reconnection is driven by loop()
  // Latency matters for call audio: no modem sleep on the backbone.
  WiFi.setSleep(false);

  IPAddress address;
  if (!address.fromString(config.apIp)) {
    address = IPAddress(10, 0, 0, 1);
  }
  const IPAddress mask(255, 255, 255, 0);
  if (!WiFi.softAPConfig(address, address, mask)) {
    LOG_W("wifi", "softAPConfig failed, default AP address will be used");
  }
  const bool started = WiFi.softAP(config.wifiSsid, config.wifiPassword, config.wifiChannel,
                                   false, 6);
  if (started) {
    LOG_I("wifi", "access point \"%s\" up on channel %u, IP %s", config.wifiSsid,
          static_cast<unsigned>(config.wifiChannel),
          WiFi.softAPIP().toString().c_str());
  } else {
    LOG_E("wifi", "access point \"%s\" failed to start", config.wifiSsid);
  }

  if (!wantsBackbone) {
    LOG_I("wifi", "backbone hub: no station interface (other stations attach to this AP)");
    return;
  }

  staConfigured_ = true;
  LOG_I("wifi", "attaching to upstream %s (\"%s\", %s) on channel %u, node %u", config.upstreamIp,
        config.upstreamSsid, config.upstreamPassword == nullptr ? "" : "auth",
        static_cast<unsigned>(config.wifiChannel), static_cast<unsigned>(config.upstreamNode));
  // The channel is given explicitly so the association never drags the access
  // point to another channel.
  WiFi.begin(config.upstreamSsid, config.upstreamPassword, config.wifiChannel);
  nextAttemptMs_ = millis() + config.linkRetryMs;
}

void WifiBackbone::loop(uint32_t nowMs) {
  if (!staConfigured_ || config_ == nullptr) {
    return;
  }
  const bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !staConnected_) {
    staConnected_ = true;
    ++reconnects_;
    logAttach(nowMs);
  } else if (!connected && staConnected_) {
    staConnected_ = false;
    LOG_W("wifi", "upstream link lost, reconnecting in %lu ms",
          static_cast<unsigned long>(config_->linkRetryMs));
    nextAttemptMs_ = nowMs + config_->linkRetryMs;
  }

  if (!connected && static_cast<int32_t>(nowMs - nextAttemptMs_) >= 0) {
    nextAttemptMs_ = nowMs + config_->linkRetryMs;
    lastAttemptMs_ = nowMs;
    LOG_I("wifi", "station interface: reconnecting to \"%s\"", config_->upstreamSsid);
    WiFi.disconnect(false, false);
    WiFi.begin(config_->upstreamSsid, config_->upstreamPassword, config_->wifiChannel);
  }
}

void WifiBackbone::logAttach(uint32_t nowMs) {
  const uint8_t currentChannel = static_cast<uint8_t>(WiFi.channel());
  LOG_I("wifi", "station interface attached: IP %s, channel %u, RSSI %d dBm",
        WiFi.localIP().toString().c_str(), static_cast<unsigned>(currentChannel),
        static_cast<int>(WiFi.RSSI()));
  if (config_ != nullptr && currentChannel != config_->wifiChannel) {
    // The AP follows the STA channel: the phones of this cell would be
    // disconnected. All three stations must therefore use the same channel.
    LOG_E("wifi",
          "channel mismatch: AP configured on %u but the upstream link is on %u; "
          "all stations must use the same channel",
          static_cast<unsigned>(config_->wifiChannel), static_cast<unsigned>(currentChannel));
  }
  lastLogMs_ = nowMs;
}

WifiBackbone::Status WifiBackbone::status() const {
  Status result;
  result.channel = config_ == nullptr ? 0 : config_->wifiChannel;
  result.upstreamNode = config_ == nullptr ? 0 : config_->upstreamNode;
  result.apIp = WiFi.softAPIP();
  result.apUp = (WiFi.getMode() & WIFI_AP) != 0;
  result.staConfigured = staConfigured_;
  result.staConnected = staConnected_;
  result.staIp = WiFi.localIP();
  result.reconnects = reconnects_;
  result.rssi = staConnected_ ? static_cast<int32_t>(WiFi.RSSI()) : 0;
  return result;
}

bool WifiBackbone::upstreamReady() const {
  if (!staConfigured_) {
    // The hub has no station interface: its backbone is always available.
    return true;
  }
  return staConnected_;
}

}  // namespace cell
