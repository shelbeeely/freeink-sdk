#pragma once

// FreeInk GPS/GNSS receiver — UART-attached NMEA 0183 module (e.g. u-blox
// NEO-6M/NEO-M8N, Quectel L80/L86).
//
// Feeds bytes from the UART described by BoardConfig::ACTIVE.gps into
// NmeaParser and exposes its latest fix. Boards without a GPS module
// (FREEINK_CAP_GPS off, or gps.module == BoardConfig::GpsModule::None) link
// stub bodies and present() returns false, mirroring Imu/Rtc.

#include <Arduino.h>

#include "NmeaParser.h"

namespace freeink {

class Gps {
 public:
  // Opens the UART described by BoardConfig::ACTIVE.gps. Returns false when
  // the active board has no GPS configured.
  bool begin();
  bool present() const { return begun_; }

  // Drains any bytes currently waiting on the UART into the parser. Returns
  // true if a new fix was completed during this call (see fix()).
  bool update();

  const GpsFix& fix() const { return parser_.fix(); }

 private:
  bool begun_ = false;
  NmeaParser parser_;
};

}  // namespace freeink

using Gps = freeink::Gps;
