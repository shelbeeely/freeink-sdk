#pragma once

#include "../FreeInkMapsCore.h"

#include <cstdio>

namespace freeink {
namespace maps {

enum class Maneuver : uint8_t {
  Straight,
  TurnLeft,
  TurnRight,
  SlightLeft,
  SlightRight,
  SharpLeft,
  SharpRight,
  UTurn,
  RoundaboutExit,
  Arrive,
};

// One step of a route, as a consumer's own routing engine would produce it —
// FreeInkMaps only renders steps, it does not compute routes.
struct NavigationStep {
  Maneuver maneuver = Maneuver::Straight;
  const char* instruction = nullptr;  // e.g. "Turn right onto Main St"
  float distanceMeters = 0.0f;        // distance from the current position to this maneuver
};

struct NavigationBannerProps {
  NavigationStep step{};
  float remainingRouteMeters = -1.0f;  // < 0 hides the remaining-distance footer
  bool useMiles = false;
  TextStyle instructionText{};
  TextStyle distanceText{};
  StyleSet styles{};
  Insets padding{8, 12, 8, 12};
  int16_t arrowSize = 28;
  int16_t gap = 10;
};

// Formats a distance for display: "80 m" / "1.2 km" (metric) or "260 ft" /
// "1.4 mi" (useMiles). `out` must be at least 16 bytes; returns `out`.
inline const char* formatDistance(float meters, bool useMiles, char* out, size_t outLen) {
  if (useMiles) {
    const float feet = meters * 3.28084f;
    if (feet < 1000.0f) {
      std::snprintf(out, outLen, "%d ft", static_cast<int>(feet + 0.5f));
    } else {
      std::snprintf(out, outLen, "%.1f mi", meters / 1609.344f);
    }
  } else {
    if (meters < 1000.0f) {
      std::snprintf(out, outLen, "%d m", static_cast<int>(meters + 0.5f));
    } else {
      std::snprintf(out, outLen, "%.1f km", meters / 1000.0f);
    }
  }
  return out;
}

namespace detail {

// Draws a maneuver glyph into `rect`: a shaft + arrowhead rotated per
// `maneuver` (Arrive draws a filled dot instead of an arrow).
inline void drawManeuverArrow(DrawTarget& target, Rect rect, Maneuver maneuver, Paint paint) {
  const int16_t cx = static_cast<int16_t>(rect.x + rect.width / 2);
  const int16_t cy = static_cast<int16_t>(rect.y + rect.height / 2);
  const double size = (rect.width < rect.height ? rect.width : rect.height) * 0.4;
  if (maneuver == Maneuver::Arrive) {
    const int16_t d = static_cast<int16_t>(size);
    target.fill(Rect{static_cast<int16_t>(cx - d / 2), static_cast<int16_t>(cy - d / 2), d, d}, paint,
                static_cast<uint8_t>(d / 2), CornersAll);
    return;
  }
  double angleDeg = 0.0;  // 0 = straight up
  switch (maneuver) {
    case Maneuver::TurnLeft: angleDeg = -90.0; break;
    case Maneuver::TurnRight: angleDeg = 90.0; break;
    case Maneuver::SlightLeft: angleDeg = -35.0; break;
    case Maneuver::SlightRight: angleDeg = 35.0; break;
    case Maneuver::SharpLeft: angleDeg = -135.0; break;
    case Maneuver::SharpRight: angleDeg = 135.0; break;
    case Maneuver::UTurn: angleDeg = 180.0; break;
    case Maneuver::Straight:
    case Maneuver::RoundaboutExit:
    default: angleDeg = 0.0; break;
  }
  const double rad = angleDeg * (kPi / 180.0);
  const double s = std::sin(rad);
  const double c = std::cos(rad);
  auto rotate = [&](double dx, double dy) -> Point {
    const double rx = dx * c - dy * s;
    const double ry = dx * s + dy * c;
    return Point{static_cast<int16_t>(cx + std::lround(rx)), static_cast<int16_t>(cy + std::lround(ry))};
  };
  const Point tail = rotate(0.0, size * 0.6);
  const Point tip = rotate(0.0, -size * 0.6);
  target.line(tail, tip, 3, paint);
  const Point headLeft = rotate(-size * 0.35, -size * 0.15);
  const Point headRight = rotate(size * 0.35, -size * 0.15);
  target.triangle(tip, headLeft, headRight, paint);
}

}  // namespace detail

// A reader-chrome-style status bar for turn-by-turn navigation: a maneuver
// glyph, the instruction text, the distance to that maneuver, and an
// optional remaining-route-distance footer.
inline void navigationBanner(DrawTarget& target, Rect rect, const NavigationBannerProps& props) {
  const auto& style = props.styles.resolve(StateNormal);
  target.fill(rect, style.background, style.radius, style.corners);
  if (style.border.kind != PaintKind::None && style.borderWidth > 0) {
    target.stroke(rect, style.border, style.borderWidth, style.radius, style.corners);
  }
  const Rect content = rect.inset(props.padding);

  const Rect arrowRect{content.x, content.y, props.arrowSize, content.height};
  detail::drawManeuverArrow(target, arrowRect, props.step.maneuver, style.foreground);

  const int16_t textX = static_cast<int16_t>(content.x + props.arrowSize + props.gap);
  const int16_t textW = static_cast<int16_t>(content.width - props.arrowSize - props.gap);

  char distBuf[16];
  formatDistance(props.step.distanceMeters, props.useMiles, distBuf, sizeof(distBuf));
  const TextStyle distStyle = textStyleWithForeground(props.distanceText, style.foreground);
  const int16_t distH = target.lineHeight(distStyle.font);
  drawText(target, Rect{textX, content.y, textW, distH}, distBuf, distStyle);

  const TextStyle instrStyle = textStyleWithForeground(props.instructionText, style.foreground);
  const int16_t instrY = static_cast<int16_t>(content.y + distH + 2);
  drawText(target, Rect{textX, instrY, textW, static_cast<int16_t>(content.bottom() - instrY)},
           props.step.instruction ? props.step.instruction : "", instrStyle);

  if (props.remainingRouteMeters >= 0.0f) {
    char remBuf[16];
    formatDistance(props.remainingRouteMeters, props.useMiles, remBuf, sizeof(remBuf));
    char footer[32];
    std::snprintf(footer, sizeof(footer), "%s remaining", remBuf);
    const TextStyle footerStyle = textStyleWithForeground(props.distanceText, style.foreground);
    const int16_t footerH = target.lineHeight(footerStyle.font);
    drawText(target, Rect{textX, static_cast<int16_t>(content.bottom() - footerH), textW, footerH}, footer,
             footerStyle);
  }
}

}  // namespace maps
}  // namespace freeink
