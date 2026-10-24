#ifndef CELL_LOG_H
#define CELL_LOG_H

#include <Arduino.h>

namespace cell {

enum LogLevel : uint8_t {
  kLogError = 0,
  kLogWarn = 1,
  kLogInfo = 2,
  kLogDebug = 3,
};

void setLogLevel(uint8_t level);
uint8_t currentLogLevel();

// printf-like serial logging. The payload of private messages and audio blocks
// is never logged: only lengths, ids and sequence numbers.
void logf(uint8_t level, const char* tag, const char* format, ...);

}  // namespace cell

#define LOG_E(tag, ...) ::cell::logf(::cell::kLogError, tag, __VA_ARGS__)
#define LOG_W(tag, ...) ::cell::logf(::cell::kLogWarn, tag, __VA_ARGS__)
#define LOG_I(tag, ...) ::cell::logf(::cell::kLogInfo, tag, __VA_ARGS__)
#define LOG_D(tag, ...) ::cell::logf(::cell::kLogDebug, tag, __VA_ARGS__)

#endif  // CELL_LOG_H
