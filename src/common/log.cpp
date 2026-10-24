#include "log.h"

#include <cstdarg>

namespace cell {
namespace {

uint8_t gLogLevel = kLogInfo;

const char* levelName(uint8_t level) {
  switch (level) {
    case kLogError: return "E";
    case kLogWarn: return "W";
    case kLogInfo: return "I";
    case kLogDebug: return "D";
    default: return "?";
  }
}

}  // namespace

void setLogLevel(uint8_t level) {
  gLogLevel = level;
}

uint8_t currentLogLevel() {
  return gLogLevel;
}

void logf(uint8_t level, const char* tag, const char* format, ...) {
  if (level > gLogLevel) {
    return;
  }
  char message[192];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  Serial.printf("[%8lu][%s][%s] %s\n", static_cast<unsigned long>(millis()),
                levelName(level), tag == nullptr ? "-" : tag, message);
}

}  // namespace cell
