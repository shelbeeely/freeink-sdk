#include "NmeaParser.h"

#include <cstdlib>
#include <cstring>

namespace freeink {
namespace {

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Splits `s` in place on ',' (each comma becomes a NUL), returning pointers
// to each field. `s` must already be NUL-terminated at the end of the last
// field (the checksum and everything after it stripped by the caller).
int splitFields(char* s, char** fields, int maxFields) {
  int n = 0;
  fields[n++] = s;
  for (char* p = s; *p; ++p) {
    if (*p == ',' && n < maxFields) {
      *p = '\0';
      fields[n++] = p + 1;
    }
  }
  return n;
}

// NMEA lat/lon fields are ddmm.mmmm (lat) / dddmm.mmmm (lon): the last two
// integer digits are always whole minutes regardless of how many degree
// digits precede them, so floor(raw / 100) isolates degrees without needing
// to know the field width up front.
bool parseCoord(const char* value, const char* hemisphere, double& outDeg) {
  if (!value || !*value || !hemisphere || !*hemisphere) return false;
  char* end = nullptr;
  const double raw = std::strtod(value, &end);
  if (end == value) return false;
  const double degrees = static_cast<double>(static_cast<long>(raw / 100.0));
  const double minutes = raw - degrees * 100.0;
  double deg = degrees + minutes / 60.0;
  if (hemisphere[0] == 'S' || hemisphere[0] == 'W') deg = -deg;
  outDeg = deg;
  return true;
}

}  // namespace

bool NmeaParser::feed(char c) {
  if (c == '$') {
    inSentence_ = true;
    len_ = 0;
    buf_[len_++] = c;
    return false;
  }
  if (!inSentence_) return false;
  if (c == '\r' || c == '\n') {
    inSentence_ = false;
    if (len_ == 0) return false;
    buf_[len_] = '\0';
    return parseSentence();
  }
  if (len_ + 1 >= kMaxSentence) {
    // Overflow: drop this sentence rather than overrun the buffer.
    inSentence_ = false;
    len_ = 0;
    return false;
  }
  buf_[len_++] = c;
  return false;
}

bool NmeaParser::parseSentence() {
  if (len_ < 6 || buf_[0] != '$') return false;
  size_t star = 0;
  bool foundStar = false;
  for (size_t i = 1; i < len_; ++i) {
    if (buf_[i] == '*') {
      star = i;
      foundStar = true;
      break;
    }
  }
  if (!foundStar || star + 2 >= len_) return false;
  const int hi = hexDigit(buf_[star + 1]);
  const int lo = hexDigit(buf_[star + 2]);
  if (hi < 0 || lo < 0) return false;
  uint8_t checksum = 0;
  for (size_t i = 1; i < star; ++i) checksum ^= static_cast<uint8_t>(buf_[i]);
  if (checksum != static_cast<uint8_t>((hi << 4) | lo)) {
    ++checksumFailures_;
    return false;
  }
  ++sentencesSeen_;

  buf_[star] = '\0';  // cut off "*CS" so the field split stops at the last real field
  const char type[4] = {buf_[3], buf_[4], buf_[5], '\0'};
  char* fields[kMaxFields];
  const int n = splitFields(buf_, fields, kMaxFields);
  if (std::strcmp(type, "RMC") == 0) return parseRmc(fields, n);
  if (std::strcmp(type, "GGA") == 0) return parseGga(fields, n);
  return false;
}

void NmeaParser::parseTime(const char* value) {
  if (!value) return;
  size_t l = 0;
  while (value[l]) ++l;
  if (l < 6) return;
  auto digitPair = [&](int i) -> uint8_t { return static_cast<uint8_t>((value[i] - '0') * 10 + (value[i + 1] - '0')); };
  fix_.hour = digitPair(0);
  fix_.minute = digitPair(2);
  fix_.second = digitPair(4);
}

bool NmeaParser::parseRmc(char** f, int n) {
  // $--RMC,time,status,lat,NS,lon,EW,speedKnots,courseDeg,date,...*CS
  if (n < 9) return false;
  parseTime(f[1]);
  const bool active = f[2][0] == 'A';
  double lat = 0.0, lon = 0.0;
  const bool posOk = parseCoord(f[3], f[4], lat) && parseCoord(f[5], f[6], lon);
  if (active && posOk) {
    fix_.latitude = lat;
    fix_.longitude = lon;
    fix_.hasPosition = true;
  } else if (!active) {
    fix_.hasPosition = false;
  }
  if (f[7][0]) fix_.speedKmh = static_cast<float>(std::strtod(f[7], nullptr) * 1.852);
  if (f[8][0]) fix_.courseDeg = static_cast<float>(std::strtod(f[8], nullptr));
  return true;
}

bool NmeaParser::parseGga(char** f, int n) {
  // $--GGA,time,lat,NS,lon,EW,fixQuality,numSat,HDOP,altitude,M,...*CS
  if (n < 10) return false;
  parseTime(f[1]);
  double lat = 0.0, lon = 0.0;
  const bool posOk = parseCoord(f[2], f[3], lat) && parseCoord(f[4], f[5], lon);
  const int quality = f[6][0] ? std::atoi(f[6]) : 0;
  if (posOk && quality > 0) {
    fix_.latitude = lat;
    fix_.longitude = lon;
    fix_.hasPosition = true;
  } else if (quality == 0) {
    fix_.hasPosition = false;
  }
  if (f[7][0]) fix_.satellites = static_cast<uint8_t>(std::atoi(f[7]));
  if (f[9][0]) fix_.altitudeMeters = static_cast<float>(std::strtod(f[9], nullptr));
  return true;
}

}  // namespace freeink
