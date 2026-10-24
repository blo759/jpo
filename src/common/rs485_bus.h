#ifndef CELL_RS485_BUS_H
#define CELL_RS485_BUS_H

#include <Arduino.h>
#include <driver/uart.h>

#include "cell_protocol.h"
#include "led_indicators.h"
#include "station_config.h"

namespace cell {

// Half duplex RS485 driver for the shared two wire bus.
//
// Transmission is fully asynchronous: the frame is handed to the UART ring
// buffer, the DE/RE line goes high, and the state machine in update() waits
// for the hardware to really finish (ring buffer empty + TX FIFO/shift register
// empty) before releasing DE/RE. The main loop therefore keeps serving Wi-Fi,
// the HTTP server and the bus arbitration during a transfer.
class Rs485Bus {
 public:
  Rs485Bus(HardwareSerial& serial, uint8_t uartPort, LedIndicators& leds)
      : serial_(serial), uartPort_(uartPort), leds_(leds) {}

  void begin(uint32_t baud) {
    pinMode(kRs485TxPin, OUTPUT);
    pinMode(kRs485RxPin, INPUT);
    pinMode(kRs485DirPin, OUTPUT);
    digitalWrite(kRs485DirPin, LOW);  // reception

    serial_.setRxBufferSize(kUartBufferSize);
    serial_.setTxBufferSize(kUartBufferSize);
    serial_.begin(baud, SERIAL_8N1, kRs485RxPin, kRs485TxPin);
    serial_.setTimeout(0);
    baud_ = baud;
    assembler_.reset();
  }

  bool isTransmitting() const { return txActive_; }

  // Queues one frame for transmission. Returns false when the transceiver is
  // still busy with the previous frame or when the frame cannot be encoded.
  bool send(const Frame& frame, uint32_t nowMs) {
    if (txActive_) {
      ++txRejected_;
      return false;
    }
    const size_t length = encodeFrame(frame, txBuffer_, sizeof(txBuffer_));
    if (length == 0U) {
      ++encodeErrors_;
      return false;
    }

    digitalWrite(kRs485DirPin, HIGH);  // drive the bus
    leds_.setTxActive(true, nowMs);
    const size_t written = serial_.write(txBuffer_, length);
    if (written != length) {
      digitalWrite(kRs485DirPin, LOW);
      leds_.setTxActive(false, nowMs);
      ++encodeErrors_;
      return false;
    }
    txBytes_ += written;

    txActive_ = true;
    txLength_ = length;
    txStartMs_ = nowMs;
    // Theoretical duration of the frame plus a generous guard, used only as a
    // safety net if the UART status never reports completion.
    txDeadlineMs_ = nowMs + static_cast<uint32_t>((length * 10U * 1000U) / baud_) + 150U;
    return true;
  }

  // Feeds the UART bytes into the COBS decoder. Returns true when one complete
  // frame has been decoded; further frames (if any) stay buffered for the next
  // call so the main loop keeps its responsiveness.
  bool poll(Frame& frame, uint32_t nowMs) {
    bool receivedByte = false;
    while (serial_.available() > 0) {
      const int value = serial_.read();
      if (value < 0) {
        break;
      }
      receivedByte = true;
      ++rxBytes_;
      const FrameAssembler::Status status =
          assembler_.push(static_cast<uint8_t>(value), frame);
      if (status == FrameAssembler::Status::kFrameReady) {
        if (receivedByte) {
          leds_.pulseRx(nowMs);  // real data arrived on the bus
        }
        ++framesReceived_;
        return true;
      }
      if (status == FrameAssembler::Status::kError) {
        ++decodeErrors_;
      }
    }
    if (receivedByte) {
      leds_.pulseRx(nowMs);
    }
    return false;
  }

  // Completes a pending transmission: waits for the real end of the UART
  // activity, then releases DE/RE and turns the green LED off.
  void update(uint32_t nowMs) {
    if (!txActive_) {
      return;
    }
    const bool ringEmpty = static_cast<size_t>(serial_.availableForWrite()) >= kUartBufferSize;
    const bool uartDone =
        uart_wait_tx_done(static_cast<uart_port_t>(uartPort_), 0) == ESP_OK;
    if ((ringEmpty && uartDone) || static_cast<int32_t>(nowMs - txDeadlineMs_) >= 0) {
      digitalWrite(kRs485DirPin, LOW);
      txActive_ = false;
      leds_.setTxActive(false, nowMs);
      ++framesSent_;
      lastFrameDurationMs_ = nowMs - txStartMs_;
    }
  }

  uint32_t framesSent() const { return framesSent_; }
  uint32_t framesReceived() const { return framesReceived_; }
  uint32_t decodeErrors() const { return decodeErrors_; }
  uint32_t txRejected() const { return txRejected_; }
  uint32_t encodeErrors() const { return encodeErrors_; }
  uint32_t lastFrameDurationMs() const { return lastFrameDurationMs_; }
  size_t lastFrameLength() const { return txLength_; }

  // Byte level counters. They are what distinguishes "the station receives no
  // byte at all" (rxBytes stays 0: wiring, transceiver, wrong direction pin) from
  // "bytes arrive but no frame can be decoded" (rxBytes grows, framesReceived
  // stays 0: baud rate, termination, A/B inversion, noise).
  uint32_t rxBytes() const { return rxBytes_; }
  uint32_t txBytes() const { return txBytes_; }
  uint32_t assemblerErrors() const { return assembler_.errorCount(); }
  uint32_t assemblerOverflows() const { return assembler_.overflowCount(); }

 private:
  HardwareSerial& serial_;
  uint8_t uartPort_;
  LedIndicators& leds_;
  FrameAssembler assembler_;
  uint8_t txBuffer_[kMaxEncodedFrameSize] = {0};
  bool txActive_ = false;
  size_t txLength_ = 0;
  uint32_t txStartMs_ = 0;
  uint32_t txDeadlineMs_ = 0;
  uint32_t baud_ = kRs485Baud;
  uint32_t framesSent_ = 0;
  uint32_t framesReceived_ = 0;
  uint32_t decodeErrors_ = 0;
  uint32_t txRejected_ = 0;
  uint32_t encodeErrors_ = 0;
  uint32_t lastFrameDurationMs_ = 0;
  uint32_t rxBytes_ = 0;
  uint32_t txBytes_ = 0;
};

}  // namespace cell

#endif  // CELL_RS485_BUS_H
