// Host-side unit tests for FreeInkMaps. FreeInkUI (which FreeInkMaps draws
// through) is freestanding C++, so this runs without a device or PlatformIO.
// Run with test/host/run.sh.

#include <FreeInkMapsCore.h>
#include <components/map-view.h>
#include <components/navigation-banner.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace freeink::maps;
using namespace freeink::ui;

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

#define CHECK_NEAR(a, b, eps)                                                                \
  do {                                                                                        \
    ++checksRun;                                                                              \
    const double va = (a);                                                                    \
    const double vb = (b);                                                                    \
    if (std::fabs(va - vb) > (eps)) {                                                         \
      ++checksFailed;                                                                          \
      std::printf("FAIL %s:%d  %s ~= %s  (%g != %g)\n", __FILE__, __LINE__, #a, #b, va, vb);   \
    }                                                                                          \
  } while (0)

// Records draw calls so tests can assert on geometry without a real panel.
// Trimmed from FreeInkUI's own test/host FakeDrawTarget.
class FakeDrawTarget : public DrawTarget {
 public:
  struct Op {
    enum Kind { Fill, Stroke, Text, Bitmap, Line, Triangle } kind;
    Rect rect;
  };
  Op ops[256]{};
  size_t opCount = 0;

  Size measureText(FontId, const char* text, TextStyle) const override {
    return Size{static_cast<int16_t>(6 * static_cast<int16_t>(std::strlen(text))), 12};
  }
  int16_t lineHeight(FontId) const override { return 12; }
  void fill(Rect rect, Paint, uint8_t, uint8_t) override { record(Op::Fill, rect); }
  void stroke(Rect rect, Paint, uint8_t, uint8_t, uint8_t) override { record(Op::Stroke, rect); }
  void line(Point from, Point to, uint8_t, Paint) override {
    record(Op::Line, Rect{from.x, from.y, static_cast<int16_t>(to.x - from.x), static_cast<int16_t>(to.y - from.y)});
  }
  void triangle(Point a, Point, Point c, Paint) override {
    record(Op::Triangle, Rect{a.x, a.y, static_cast<int16_t>(c.x - a.x), static_cast<int16_t>(c.y - a.y)});
  }
  void text(Rect rect, const char*, TextStyle) override { record(Op::Text, rect); }
  void bitmap(Rect rect, BitmapRef, BitmapMode, Paint, Rotation) override { record(Op::Bitmap, rect); }

  size_t countKind(Op::Kind kind) const {
    size_t n = 0;
    for (size_t i = 0; i < opCount; ++i)
      if (ops[i].kind == kind) ++n;
    return n;
  }

 private:
  void record(Op::Kind kind, Rect rect) {
    if (opCount < sizeof(ops) / sizeof(ops[0])) ops[opCount++] = Op{kind, rect};
  }
};

void testGeoToTilePixelZoom0() {
  // At zoom 0 the whole world is one 256x256 tile; (0,0) lat/lon sits at its
  // exact center.
  const TilePixel p = geoToTilePixel(GeoPoint{0.0, 0.0}, 0, 256);
  CHECK(p.tileX == 0 && p.tileY == 0);
  CHECK_NEAR(p.pixelX, 128.0, 1e-6);
  CHECK_NEAR(p.pixelY, 128.0, 1e-6);
}

void testFindTile() {
  MapTile tiles[2];
  tiles[0] = MapTile{16, 100, 200, BitmapRef{}};
  tiles[1] = MapTile{16, 101, 200, BitmapRef{}};
  CHECK(findTile(tiles, 2, 16, 100, 200) == &tiles[0]);
  CHECK(findTile(tiles, 2, 16, 101, 200) == &tiles[1]);
  CHECK(findTile(tiles, 2, 16, 999, 200) == nullptr);
  CHECK(findTile(tiles, 2, 15, 100, 200) == nullptr);  // wrong zoom
}

void testGeoDistanceMeters() {
  // One degree of latitude is ~111.19 km everywhere on a sphere.
  const double d = geoDistanceMeters(GeoPoint{0.0, 0.0}, GeoPoint{1.0, 0.0});
  CHECK_NEAR(d, 111194.9, 50.0);
  CHECK_NEAR(geoDistanceMeters(GeoPoint{10.0, 20.0}, GeoPoint{10.0, 20.0}), 0.0, 1e-6);
}

void testGeoBearingDeg() {
  CHECK_NEAR(geoBearingDeg(GeoPoint{0.0, 0.0}, GeoPoint{1.0, 0.0}), 0.0, 0.5);    // due north
  CHECK_NEAR(geoBearingDeg(GeoPoint{0.0, 0.0}, GeoPoint{0.0, 1.0}), 90.0, 0.5);   // due east, on the equator
  CHECK_NEAR(geoBearingDeg(GeoPoint{0.0, 0.0}, GeoPoint{-1.0, 0.0}), 180.0, 0.5); // due south
}

void testMapViewDrawsMatchingTile() {
  FakeDrawTarget target;
  const GeoPoint center{47.6062, -122.3321};  // Seattle
  const TilePixel px = geoToTilePixel(center, 16, 256);

  uint8_t pixel = 0xFF;
  MapTile tile{16, px.tileX, px.tileY, BitmapRef{&pixel, 256, 256, BitmapFormat::BW1, false}};

  MapViewProps props;
  props.tiles = &tile;
  props.tileCount = 1;
  props.zoom = 16;
  props.tileSizePx = 256;
  props.center = center;
  props.position = center;
  props.routePoints = nullptr;
  props.routePointCount = 0;

  mapView(target, Rect{0, 0, 300, 300}, props);
  CHECK(target.countKind(FakeDrawTarget::Op::Bitmap) >= 1);
  CHECK(target.countKind(FakeDrawTarget::Op::Triangle) == 1);  // position marker
}

void testMapViewDrawsRoute() {
  FakeDrawTarget target;
  const GeoPoint a{47.6062, -122.3321};
  const GeoPoint b{47.6070, -122.3310};
  const GeoPoint route[2] = {a, b};

  MapViewProps props;
  props.tiles = nullptr;
  props.tileCount = 0;
  props.center = a;
  props.position = a;
  props.showPosition = false;
  props.routePoints = route;
  props.routePointCount = 2;

  mapView(target, Rect{0, 0, 300, 300}, props);
  CHECK(target.countKind(FakeDrawTarget::Op::Line) == 1);
  CHECK(target.countKind(FakeDrawTarget::Op::Triangle) == 0);  // showPosition off
}

void testFormatDistance() {
  char buf[16];
  CHECK(std::strcmp(formatDistance(80.0f, false, buf, sizeof(buf)), "80 m") == 0);
  CHECK(std::strcmp(formatDistance(1500.0f, false, buf, sizeof(buf)), "1.5 km") == 0);
  CHECK(std::strcmp(formatDistance(200.0f, true, buf, sizeof(buf)), "656 ft") == 0);
  CHECK(std::strcmp(formatDistance(2000.0f, true, buf, sizeof(buf)), "1.2 mi") == 0);
}

void testNavigationBanner() {
  FakeDrawTarget target;
  NavigationBannerProps props;
  props.step.maneuver = Maneuver::TurnRight;
  props.step.instruction = "Turn right onto Main St";
  props.step.distanceMeters = 150.0f;
  props.remainingRouteMeters = 2400.0f;

  navigationBanner(target, Rect{0, 0, 480, 60}, props);
  CHECK(target.countKind(FakeDrawTarget::Op::Text) == 3);      // distance + instruction + remaining footer
  CHECK(target.countKind(FakeDrawTarget::Op::Line) == 1);      // arrow shaft (TurnRight is not Arrive)
  CHECK(target.countKind(FakeDrawTarget::Op::Triangle) == 1);  // arrowhead

  FakeDrawTarget arriveTarget;
  NavigationBannerProps arriveProps;
  arriveProps.step.maneuver = Maneuver::Arrive;
  arriveProps.step.instruction = "You have arrived";
  navigationBanner(arriveTarget, Rect{0, 0, 480, 60}, arriveProps);
  CHECK(arriveTarget.countKind(FakeDrawTarget::Op::Line) == 0);
  CHECK(arriveTarget.countKind(FakeDrawTarget::Op::Triangle) == 0);
  CHECK(arriveTarget.countKind(FakeDrawTarget::Op::Fill) >= 1);  // arrive dot + banner background
}

}  // namespace

int main() {
  testGeoToTilePixelZoom0();
  testFindTile();
  testGeoDistanceMeters();
  testGeoBearingDeg();
  testMapViewDrawsMatchingTile();
  testMapViewDrawsRoute();
  testFormatDistance();
  testNavigationBanner();

  std::printf("%d checks, %d failed\n", checksRun, checksFailed);
  return checksFailed == 0 ? 0 : 1;
}
