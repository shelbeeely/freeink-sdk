#include "Gps.h"

#include <BoardConfig.h>

#if FREEINK_CAP_GPS

#include <soc/soc_caps.h>

namespace freeink {
namespace {

HardwareSerial& gpsSerial(uint8_t port) {
#if SOC_UART_NUM > 2
  if (port == 2) return Serial2;
#endif
#if SOC_UART_NUM > 1
  if (port == 1) return Serial1;
#endif
  return Serial;
}

}  // namespace

bool Gps::begin() {
  begun_ = false;
  const auto& g = BoardConfig::ACTIVE.gps;
  if (g.module == BoardConfig::GpsModule::None) return false;
  if (g.rxPin < 0 || g.baud == 0) return false;
  if (g.enable != BoardConfig::PIN_UNASSIGNED) {
    pinMode(g.enable, OUTPUT);
    digitalWrite(g.enable, g.enableActiveHigh ? HIGH : LOW);
  }
  gpsSerial(g.uartPort).begin(g.baud, SERIAL_8N1, g.rxPin, g.txPin);
  begun_ = true;
  return true;
}

bool Gps::update() {
  if (!begun_) return false;
  auto& serial = gpsSerial(BoardConfig::ACTIVE.gps.uartPort);
  bool updated = false;
  while (serial.available() > 0) {
    if (parser_.feed(static_cast<char>(serial.read()))) updated = true;
  }
  return updated;
}

}  // namespace freeink

#else  // FREEINK_CAP_GPS off — GPS absent.

namespace freeink {
bool Gps::begin() { return false; }
bool Gps::update() { return false; }
}  // namespace freeink

#endif  // FREEINK_CAP_GPS
