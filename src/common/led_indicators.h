#ifndef CELL_LED_INDICATORS_H
#define CELL_LED_INDICATORS_H

#include <Arduino.h>

namespace cell {

// Non blocking LED driver.
//  * green (GPIO25): lit while the RS485 transceiver really drives the bus,
//    held a few milliseconds afterwards so a short frame stays visible;
//  * red (GPIO26): pulses on every byte actually received from the bus.
class LedIndicators {
 public:
  static constexpr uint32_t kVisiblePulseMs = 45;

  void begin(uint8_t txPin, uint8_t rxPin) {
    txPin_ = txPin;
    rxPin_ = rxPin;
    pinMode(txPin_, OUTPUT);
    pinMode(rxPin_, OUTPUT);
    digitalWrite(txPin_, LOW);
    digitalWrite(rxPin_, LOW);
  }

  void setTxActive(bool active, uint32_t nowMs) {
    txActive_ = active;
    if (active) {
      digitalWrite(txPin_, HIGH);
      txUntilMs_ = nowMs + kVisiblePulseMs;
    } else {
      txUntilMs_ = nowMs + kVisiblePulseMs;
    }
  }

  void pulseRx(uint32_t nowMs) {
    digitalWrite(rxPin_, HIGH);
    rxUntilMs_ = nowMs + kVisiblePulseMs;
  }

  void update(uint32_t nowMs) {
    if (!txActive_ && static_cast<int32_t>(nowMs - txUntilMs_) >= 0) {
      digitalWrite(txPin_, LOW);
    }
    if (static_cast<int32_t>(nowMs - rxUntilMs_) >= 0) {
      digitalWrite(rxPin_, LOW);
    }
  }

  void allOff() {
    txActive_ = false;
    txUntilMs_ = 0;
    rxUntilMs_ = 0;
    digitalWrite(txPin_, LOW);
    digitalWrite(rxPin_, LOW);
  }

  bool txActive() const { return txActive_; }

 private:
  uint8_t txPin_ = 25;
  uint8_t rxPin_ = 26;
  bool txActive_ = false;
  uint32_t txUntilMs_ = 0;
  uint32_t rxUntilMs_ = 0;
};

}  // namespace cell

#endif  // CELL_LED_INDICATORS_H
