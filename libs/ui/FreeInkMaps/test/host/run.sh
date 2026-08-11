#!/bin/sh
# Builds and runs the FreeInkMaps host tests. No device or PlatformIO needed —
# FreeInkMaps and the FreeInkUI it draws through are both freestanding C++.
set -e
cd "$(dirname "$0")"
BUILD_DIR="${TMPDIR:-/tmp}/freeinkmaps-tests"
mkdir -p "$BUILD_DIR"
c++ -std=c++17 -Wall -Wextra -Werror \
  -I../../include -I../../../FreeInkUI/include \
  ../../../FreeInkUI/src/FreeInkUI.cpp test_freeinkmaps.cpp \
  -o "$BUILD_DIR/test_freeinkmaps"
"$BUILD_DIR/test_freeinkmaps"
