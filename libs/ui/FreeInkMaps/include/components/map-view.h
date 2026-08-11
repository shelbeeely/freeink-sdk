#pragma once

#include "../FreeInkMapsCore.h"

namespace freeink {
namespace maps {

struct MapViewProps {
  const MapTile* tiles = nullptr;
  uint16_t tileCount = 0;
  uint8_t zoom = 16;
  uint16_t tileSizePx = 256;
  GeoPoint center{};    // geo point rendered at the center of `rect`
  GeoPoint position{};  // current position; drawn when showPosition is true
  float headingDeg = 0.0f;  // degrees clockwise from north; orients the position marker only (see file header)
  bool showPosition = true;
  const GeoPoint* routePoints = nullptr;
  uint16_t routePointCount = 0;
  Paint background = Paint::solid(Color::White);
  Paint routePaint = Paint::solid(Color::Black);
  Paint positionPaint = Paint::solid(Color::Black);
  uint8_t routeWidth = 3;
  uint8_t positionMarkerSize = 10;
};

namespace detail {

inline Point mapTileOrigin(Rect rect, TilePixel centerPx) {
  return Point{static_cast<int16_t>(rect.x + rect.width / 2 - std::lround(centerPx.pixelX)),
               static_cast<int16_t>(rect.y + rect.height / 2 - std::lround(centerPx.pixelY))};
}

inline Point mapProjectToScreen(uint8_t zoom, uint16_t tileSizePx, TilePixel centerPx, Point origin, GeoPoint geo) {
  const TilePixel p = geoToTilePixel(geo, zoom, tileSizePx);
  const int32_t dTileX = p.tileX - centerPx.tileX;
  const int32_t dTileY = p.tileY - centerPx.tileY;
  return Point{static_cast<int16_t>(origin.x + dTileX * tileSizePx + std::lround(p.pixelX)),
               static_cast<int16_t>(origin.y + dTileY * tileSizePx + std::lround(p.pixelY))};
}

}  // namespace detail

// Draws a north-up grid of pre-rendered raster tiles covering `rect`, plus an
// optional route polyline and a heading-oriented position marker. Tiles are
// standard Web Mercator XYZ (see geoToTilePixel); missing tiles (not found in
// `tiles`) draw as `background`, so sparse compiled-in coverage is fine.
// Tiles may extend past `rect`'s edges at the boundary — like every other
// FreeInkUI component, mapView() relies on the DrawTarget clipping to the
// physical screen itself.
inline void mapView(DrawTarget& target, Rect rect, const MapViewProps& props) {
  target.fill(rect, props.background);
  if (props.tileSizePx == 0 || rect.empty()) return;

  const TilePixel centerPx = geoToTilePixel(props.center, props.zoom, props.tileSizePx);
  const Point origin = detail::mapTileOrigin(rect, centerPx);

  const int32_t firstDx = static_cast<int32_t>(std::floor(static_cast<double>(rect.x - origin.x) / props.tileSizePx));
  const int32_t lastDx =
      static_cast<int32_t>(std::floor(static_cast<double>(rect.right() - 1 - origin.x) / props.tileSizePx));
  const int32_t firstDy = static_cast<int32_t>(std::floor(static_cast<double>(rect.y - origin.y) / props.tileSizePx));
  const int32_t lastDy =
      static_cast<int32_t>(std::floor(static_cast<double>(rect.bottom() - 1 - origin.y) / props.tileSizePx));

  for (int32_t dy = firstDy; dy <= lastDy; ++dy) {
    for (int32_t dx = firstDx; dx <= lastDx; ++dx) {
      const MapTile* tile =
          findTile(props.tiles, props.tileCount, props.zoom, centerPx.tileX + dx, centerPx.tileY + dy);
      if (!tile || !tile->bitmap) continue;
      const Rect dstRect{static_cast<int16_t>(origin.x + dx * props.tileSizePx),
                         static_cast<int16_t>(origin.y + dy * props.tileSizePx),
                         static_cast<int16_t>(props.tileSizePx), static_cast<int16_t>(props.tileSizePx)};
      target.bitmap(dstRect, tile->bitmap, BitmapMode::Center);
    }
  }

  if (props.routePoints && props.routePointCount >= 2) {
    Point prev = detail::mapProjectToScreen(props.zoom, props.tileSizePx, centerPx, origin, props.routePoints[0]);
    for (uint16_t i = 1; i < props.routePointCount; ++i) {
      const Point next = detail::mapProjectToScreen(props.zoom, props.tileSizePx, centerPx, origin, props.routePoints[i]);
      target.line(prev, next, props.routeWidth, props.routePaint);
      prev = next;
    }
  }

  if (props.showPosition) {
    const Point at = detail::mapProjectToScreen(props.zoom, props.tileSizePx, centerPx, origin, props.position);
    const double rad = static_cast<double>(props.headingDeg) * (kPi / 180.0);
    const double s = std::sin(rad);
    const double c = std::cos(rad);
    const double size = props.positionMarkerSize;
    auto rotate = [&](double dx, double dy) -> Point {
      const double rx = dx * c - dy * s;
      const double ry = dx * s + dy * c;
      return Point{static_cast<int16_t>(at.x + std::lround(rx)), static_cast<int16_t>(at.y + std::lround(ry))};
    };
    const Point tip = rotate(0.0, -size);
    const Point left = rotate(-size * 0.6, size * 0.6);
    const Point right = rotate(size * 0.6, size * 0.6);
    target.triangle(tip, left, right, props.positionPaint);
  }
}

}  // namespace maps
}  // namespace freeink
