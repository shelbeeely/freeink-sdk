#pragma once

// Freestanding NMEA 0183 sentence parser (no Arduino/ESP-IDF dependency), so
// it runs identically on-device and in host tests (see test/host/run.sh).
// Feed it raw bytes from a GPS/GNSS UART one at a time; it accumulates a
// line, validates the checksum, and updates the latest fix from $--RMC /
// $--GGA sentences. The talker ID (GP/GN/GL/GA/GB/GQ/...) is ignored, so any
// constellation combination a module emits is accepted the same way.

#include <cstddef>
#include <cstdint>

namespace freeink {

struct GpsFix {
  double latitude = 0.0;    // degrees, +N / -S
  double longitude = 0.0;   // degrees, +E / -W
  float altitudeMeters = 0.0f;
  float speedKmh = 0.0f;
  float courseDeg = 0.0f;  // course over ground, 0 = north, clockwise
  uint8_t satellites = 0;
  uint8_t hour = 0, minute = 0, second = 0;  // UTC time of fix
  // True once a sentence has reported an active fix (RMC status 'A' or GGA
  // fix quality > 0); false before the first fix and after the receiver
  // reports it lost latitude/longitude are then stale, not "0,0".
  bool hasPosition = false;
};

class NmeaParser {
 public:
  // Feeds one byte. Returns true exactly when a complete, checksum-valid RMC
  // or GGA sentence was just parsed and updated the fix. Malformed or
  // checksum-failed sentences are dropped silently (returns false); watch
  // sentencesSeen()/checksumFailures() for diagnostics.
  bool feed(char c);

  const GpsFix& fix() const { return fix_; }
  uint32_t sentencesSeen() const { return sentencesSeen_; }
  uint32_t checksumFailures() const { return checksumFailures_; }

 private:
  static constexpr size_t kMaxSentence = 96;
  static constexpr int kMaxFields = 20;

  bool parseSentence();
  bool parseRmc(char** fields, int count);
  bool parseGga(char** fields, int count);
  void parseTime(const char* value);

  char buf_[kMaxSentence];
  size_t len_ = 0;
  bool inSentence_ = false;
  GpsFix fix_{};
  uint32_t sentencesSeen_ = 0;
  uint32_t checksumFailures_ = 0;
};

}  // namespace freeink
