// Host-side unit tests for NmeaParser. The parser is freestanding C++, so
// its sentence framing/checksum/field decode is verified here without a UART
// or device in the loop. Run with test/host/run.sh.

#include <NmeaParser.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int checksRun = 0;
int checksFailed = 0;

#define CHECK(cond)                                                 \
  do {                                                               \
    ++checksRun;                                                     \
    if (!(cond)) {                                                   \
      ++checksFailed;                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
    }                                                                \
  } while (0)

#define CHECK_NEAR(a, b, eps)                                                                      \
  do {                                                                                              \
    ++checksRun;                                                                                    \
    const double va = (a);                                                                          \
    const double vb = (b);                                                                          \
    if (std::fabs(va - vb) > (eps)) {                                                               \
      ++checksFailed;                                                                                \
      std::printf("FAIL %s:%d  %s ~= %s  (%g != %g)\n", __FILE__, __LINE__, #a, #b, va, vb);         \
    }                                                                                                \
  } while (0)

void feedLine(freeink::NmeaParser& parser, const char* line) {
  for (const char* p = line; *p; ++p) parser.feed(*p);
}

void testGga() {
  freeink::NmeaParser parser;
  const bool updated = [&] {
    bool any = false;
    // Classic textbook GGA example (Wikipedia's NMEA 0183 article).
    const char* line = "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";
    for (const char* p = line; *p; ++p)
      if (parser.feed(*p)) any = true;
    return any;
  }();
  CHECK(updated);
  const freeink::GpsFix& fix = parser.fix();
  CHECK(fix.hasPosition);
  CHECK_NEAR(fix.latitude, 48.1173, 1e-4);
  CHECK_NEAR(fix.longitude, 11.516667, 1e-4);
  CHECK_NEAR(fix.altitudeMeters, 545.4, 1e-3);
  CHECK(fix.satellites == 8);
  CHECK(fix.hour == 12 && fix.minute == 35 && fix.second == 19);
  CHECK(parser.sentencesSeen() == 1);
  CHECK(parser.checksumFailures() == 0);
}

void testRmc() {
  freeink::NmeaParser parser;
  // Classic textbook RMC example (same source position, different fields).
  feedLine(parser, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n");
  const freeink::GpsFix& fix = parser.fix();
  CHECK(fix.hasPosition);
  CHECK_NEAR(fix.latitude, 48.1173, 1e-4);
  CHECK_NEAR(fix.longitude, 11.516667, 1e-4);
  CHECK_NEAR(fix.speedKmh, 22.4 * 1.852, 1e-3);
  CHECK_NEAR(fix.courseDeg, 84.4, 1e-3);
}

void testVoidRmcClearsPosition() {
  freeink::NmeaParser parser;
  feedLine(parser, "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A\r\n");
  CHECK(parser.fix().hasPosition);
  // Same sentence with status flipped to Void ('V') — recomputed checksum.
  feedLine(parser, "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*7D\r\n");
  CHECK(!parser.fix().hasPosition);
}

void testChecksumFailureIsDropped() {
  freeink::NmeaParser parser;
  feedLine(parser, "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*00\r\n");
  CHECK(!parser.fix().hasPosition);
  CHECK(parser.sentencesSeen() == 0);
  CHECK(parser.checksumFailures() == 1);
}

void testUnknownSentenceIgnored() {
  freeink::NmeaParser parser;
  // $GPGSV satellites-in-view sentence, valid checksum, not RMC/GGA.
  const bool updated = [&] {
    bool any = false;
    const char* line = "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74\r\n";
    for (const char* p = line; *p; ++p)
      if (parser.feed(*p)) any = true;
    return any;
  }();
  CHECK(!updated);
  CHECK(parser.sentencesSeen() == 1);  // checksum-valid, just not a fix sentence
}

void testTalkerIdIgnored() {
  freeink::NmeaParser parser;
  // GNGGA (multi-constellation talker) instead of GPGGA, same fields.
  feedLine(parser, "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*59\r\n");
  CHECK(parser.fix().hasPosition);
}

}  // namespace

int main() {
  testGga();
  testRmc();
  testVoidRmcClearsPosition();
  testChecksumFailureIsDropped();
  testUnknownSentenceIgnored();
  testTalkerIdIgnored();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
