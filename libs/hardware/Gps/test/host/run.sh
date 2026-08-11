#!/bin/sh
# Builds and runs the NmeaParser host tests. No device or PlatformIO needed —
# the parser is freestanding C++.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeink-gps-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror -I../../include ../../src/NmeaParser.cpp test_nmea_parser.cpp -o "$BUILD_DIR/test_nmea_parser"
"$BUILD_DIR/test_nmea_parser"
