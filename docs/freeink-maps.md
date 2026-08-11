# FreeInkMaps + Gps

Offline map display and turn-by-turn navigation chrome for FreeInk devices, in
two small libraries:

- **`libs/hardware/Gps`** — reads a GPS/GNSS receiver over UART (NMEA 0183)
  and exposes a `GpsFix` (latitude/longitude/altitude/speed/course).
- **`libs/ui/FreeInkMaps`** — draws pre-rendered offline map tiles, a route,
  a position marker, and a turn-by-turn instruction banner through a
  FreeInkUI `DrawTarget`.

Neither library computes routes. FreeInkMaps renders map tiles and whatever
route/step data a consumer's own routing logic (or a pre-baked route) hands
it; it does not fetch tiles, geocode addresses, or run a router itself.

## Gps

No current FreeInk board wires a GPS module on-glass, so `Gps` is opt-in like
`SecureNet`'s TLS 1.3 stack: gated by `FREEINK_CAP_GPS` (default off) and by
`BoardConfig::ACTIVE.gps.module` (default `GpsModule::None`).

Wire a UART GPS module (e.g. u-blox NEO-6M/NEO-M8N, Quectel L80/L86) to a
spare UART, then set the board profile:

```cpp
// In your own BoardProfile, or a board fork that adds one:
constexpr BoardConfig::GpsConfig MY_GPS = {
    BoardConfig::GpsModule::Nmea,
    /* uartPort */ 1,
    /* rxPin */ 17,
    /* txPin */ 18,
    /* enable */ BoardConfig::PIN_UNASSIGNED,
    /* enableActiveHigh */ true,
    /* baud */ 9600,
};
```

and build with `-DFREEINK_CAP_GPS=1`. Then:

```cpp
#include <Gps.h>

freeink::Gps gps;
gps.begin();

void loop() {
  if (gps.update()) {
    const freeink::GpsFix& fix = gps.fix();
    if (fix.hasPosition) {
      // fix.latitude, fix.longitude, fix.courseDeg, fix.speedKmh, ...
    }
  }
}
```

`NmeaParser` (the sentence decoder `Gps` wraps) is freestanding C++ with no
Arduino dependency, so it has its own host tests
(`libs/hardware/Gps/test/host/run.sh`) independent of any UART or board.

## FreeInkMaps

### Map tiles

Map tiles are compiled-in `BitmapRef` data at standard Web Mercator XYZ
addresses (`zoom`/`tileX`/`tileY`) — the same scheme OpenStreetMap,
OpenFreeMap, and most slippy-map tools use — so tiles line up correctly and
tools that export XYZ tile folders can feed FreeInkMaps.

There is no on-device tile fetch, PNG decode, or heap allocation: tiles are
generated ahead of time with `libs/ui/FreeInkMaps/tools/gen_maptiles.py`
(mirrors `gen_icons.py`/`gen_font.py`) from a folder of 1-bit tile PNGs laid
out as `{z}/{x}/{y}.png`:

```sh
python3 libs/ui/FreeInkMaps/tools/gen_maptiles.py \
    --tiles-dir path/to/tiles/osm-eink \
    --zoom 16 \
    --out generated_maptiles.h
```

A convenient tile source is
[HarukiToreda's E-ink Map Tiles](https://github.com/HarukiToreda/E-ink-Map-Tiles),
a local desktop app that renders OpenFreeMap vector data into e-paper-ready
PNG tiles (grayscale, mono, or InkHUD's dithered mode) in exactly this XYZ
folder layout, with pan/zoom preview and coverage/flash-budget estimates.
Export with **mode `mono`** (plain black/white PNGs, not the LZ4-packed
`MapTile.h`/`MapTile.bin` that tool's own InkHUD export produces — FreeInkMaps
reads the plain PNG tile folder) and point `--tiles-dir` at the resulting
`tiles/<style>/` folder. **Carry that tool's required OpenStreetMap /
OpenFreeMap attribution with anything you ship** — `gen_maptiles.py` does not
add it for you; see that project's README's Attribution section.

Only generate the tiles you actually need (`--zoom` plus whatever
`{x}/{y}` bounding box covers your area) — like Icons, "generate only what you
use" keeps flash usage down. Tiles default to 256x256px 1bpp
(`BitmapFormat::BW1`); override with `--tile-size` to match a different
export.

### `mapView`

```cpp
#include <components/map-view.h>
#include "generated_maptiles.h"  // kMapTiles / kMapTilesCount, from gen_maptiles.py

using namespace freeink::maps;

MapViewProps props;
props.tiles = kMapTiles;
props.tileCount = kMapTilesCount;
props.zoom = 16;
props.center = GeoPoint{gps.fix().latitude, gps.fix().longitude};
props.position = props.center;
props.headingDeg = gps.fix().courseDeg;

mapView(target, screenRect, props);  // target: a freeink::ui::DisplayTarget (or any DrawTarget)
```

`mapView()` draws a **north-up** grid of tiles around `center`, an optional
route polyline (`routePoints`/`routePointCount`), and a position marker that
rotates to `headingDeg`. The map itself does not rotate to match heading —
re-rasterizing tiles every frame is not something an e-paper refresh budget
affords, so only the (cheap, vector) marker turns. Tiles missing from the
compiled-in set draw as `background`, so partial/sparse coverage degrades
gracefully instead of crashing.

`FreeInkMapsCore.h` also exposes `geoDistanceMeters()` (haversine) and
`geoBearingDeg()` (initial bearing) as small freestanding helpers for a
consumer's own route-progress bookkeeping — advancing to the next
`NavigationStep`, deciding when a maneuver has been reached, etc.

### `navigationBanner`

A reader-chrome-style status bar for turn-by-turn instructions:

```cpp
#include <components/navigation-banner.h>

using namespace freeink::maps;

NavigationBannerProps props;
props.step = {Maneuver::TurnRight, "Turn right onto Main St", 150.0f};
props.remainingRouteMeters = 2400.0f;

navigationBanner(target, bannerRect, props);
```

Draws a maneuver glyph (rotated arrow, or a filled dot for `Arrive`), the
instruction text, the distance to that maneuver, and an optional
remaining-route-distance footer. `formatDistance()` is exposed standalone for
reuse elsewhere (`"80 m"` / `"1.2 km"`, or `"260 ft"` / `"1.4 mi"` with
`useMiles`).

`NavigationStep`/`Maneuver` are plain data — a consumer's own routing engine
(on-device, or a route baked in at build time) fills them in; FreeInkMaps only
renders them.

### Host tests

Both `FreeInkMapsCore.h`'s tile math and the two components are freestanding
C++17 like FreeInkUI, so they're covered by host tests with no device or
PlatformIO involved:

```sh
sh libs/ui/FreeInkMaps/test/host/run.sh
```
