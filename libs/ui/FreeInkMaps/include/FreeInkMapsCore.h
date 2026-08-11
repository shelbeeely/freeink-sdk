#pragma once

// FreeInk SDK — offline raster map tiles + turn-by-turn navigation chrome.
//
// FreeInkMaps draws pre-rendered 1-bpp map tiles (standard Web Mercator XYZ
// scheme) and navigation UI onto a FreeInkUI DrawTarget. Tiles are compiled-in
// const BitmapRef data — like FreeInkUI's borrowed strings/assets, there is no
// on-device tile-server fetch, decode, or heap allocation. Generate them with
// tools/gen_maptiles.py from a folder of 1-bit PNG tiles, the same "generate
// only what you use" pattern as the Icon and font tooling. A convenient tile
// source is HarukiToreda's E-ink Map Tiles desktop exporter
// (https://github.com/HarukiToreda/E-ink-Map-Tiles), which renders OpenFreeMap
// data to e-paper-ready PNG tiles in the same XYZ scheme — remember to carry
// its required OpenStreetMap/OpenFreeMap attribution with anything you ship.
//
// mapView() (components/map-view.h) draws a north-up grid of tiles around a
// geo center plus an optional route and position marker; it does not rotate
// the map to match heading — re-rasterizing tiles per frame is not something
// an e-paper refresh budget affords, so only the (cheap, vector) position
// marker rotates. navigationBanner() (components/navigation-banner.h) draws
// turn-by-turn instruction chrome from steps a consumer's own routing engine
// produces; FreeInkMaps does not compute routes itself.

#include <FreeInkUICore.h>

#include <cmath>
#include <cstdint>

namespace freeink {
namespace maps {

using ui::BitmapFormat;
using ui::BitmapMode;
using ui::BitmapRef;
using ui::Color;
using ui::CornersAll;
using ui::DrawTarget;
using ui::Insets;
using ui::Paint;
using ui::PaintKind;
using ui::Point;
using ui::Rect;
using ui::State;
using ui::StateNormal;
using ui::StyleSet;
using ui::TextStyle;
using ui::drawText;
using ui::textStyleWithForeground;

constexpr double kPi = 3.14159265358979323846;

struct GeoPoint {
  double lat = 0.0;  // degrees, +N / -S
  double lon = 0.0;  // degrees, +E / -W
};

// A single pre-rendered raster tile at a Web Mercator XYZ address. `bitmap`
// is expected to be tileSizePx x tileSizePx (see MapViewProps::tileSizePx);
// mismatched sizes still draw (BitmapMode::Center), just misaligned with
// neighboring tiles.
struct MapTile {
  uint8_t zoom = 0;
  int32_t tileX = 0;
  int32_t tileY = 0;
  BitmapRef bitmap{};
};

// A geo point's position within the Web Mercator tile grid at a given zoom:
// which tile it falls in, and its pixel offset within that tile ([0, tileSizePx)).
struct TilePixel {
  int32_t tileX = 0;
  int32_t tileY = 0;
  double pixelX = 0.0;
  double pixelY = 0.0;
};

// Standard Web Mercator slippy-map tile math (the XYZ scheme used by
// OpenStreetMap/OpenFreeMap/MapTiler and by tools like the E-ink Map Tiles
// exporter linked above), so tiles from any such source line up correctly.
inline TilePixel geoToTilePixel(GeoPoint p, uint8_t zoom, uint16_t tileSizePx) {
  const double n = std::exp2(static_cast<double>(zoom));
  const double x = (p.lon + 180.0) / 360.0 * n;
  const double latRad = p.lat * (kPi / 180.0);
  const double y = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / kPi) / 2.0 * n;
  const double tileXf = std::floor(x);
  const double tileYf = std::floor(y);
  return TilePixel{static_cast<int32_t>(tileXf), static_cast<int32_t>(tileYf),
                    (x - tileXf) * tileSizePx, (y - tileYf) * tileSizePx};
}

// Linear search over a compiled-in tile set. Fine for the small, hand-picked
// coverage areas FreeInkMaps targets (dozens to low hundreds of tiles); it is
// not a general tile cache.
inline const MapTile* findTile(const MapTile* tiles, uint16_t count, uint8_t zoom, int32_t tileX, int32_t tileY) {
  for (uint16_t i = 0; i < count; ++i) {
    const MapTile& t = tiles[i];
    if (t.zoom == zoom && t.tileX == tileX && t.tileY == tileY) return &t;
  }
  return nullptr;
}

// Great-circle distance in meters (haversine, WGS84 mean radius). Handy for a
// consumer's own route-progress / distance-to-maneuver bookkeeping.
inline double geoDistanceMeters(GeoPoint a, GeoPoint b) {
  constexpr double kEarthRadiusM = 6371000.0;
  const double lat1 = a.lat * (kPi / 180.0);
  const double lat2 = b.lat * (kPi / 180.0);
  const double dLat = (b.lat - a.lat) * (kPi / 180.0);
  const double dLon = (b.lon - a.lon) * (kPi / 180.0);
  const double sinDLat = std::sin(dLat / 2.0);
  const double sinDLon = std::sin(dLon / 2.0);
  const double h = sinDLat * sinDLat + std::cos(lat1) * std::cos(lat2) * sinDLon * sinDLon;
  const double clamped = h < 1.0 ? h : 1.0;
  return 2.0 * kEarthRadiusM * std::asin(std::sqrt(clamped));
}

// Initial bearing from a to b, degrees clockwise from north, in [0, 360).
inline float geoBearingDeg(GeoPoint a, GeoPoint b) {
  const double lat1 = a.lat * (kPi / 180.0);
  const double lat2 = b.lat * (kPi / 180.0);
  const double dLon = (b.lon - a.lon) * (kPi / 180.0);
  const double y = std::sin(dLon) * std::cos(lat2);
  const double x = std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dLon);
  double deg = std::atan2(y, x) * (180.0 / kPi);
  if (deg < 0.0) deg += 360.0;
  return static_cast<float>(deg);
}

}  // namespace maps
}  // namespace freeink
