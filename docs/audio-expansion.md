# Audio Expansion

## Overview

FreeInk's audio subsystem today covers WAV and (opt-in) MP3 output through
`AudioManager`, PDM microphone capture through `Microphone`, and
provider-agnostic TLS-backed bridges (`SecureNet`, `TtsClient`) for reaching a
cloud service without the SDK hardcoding any vendor. This doc is a short
near-term roadmap for what's next: a speech-to-text counterpart to
`TtsClient`, a dedicated FreeInkUI suite for audio, content sources so
audiobooks/music/podcasts are all actually usable (not just narration), and a
couple of smaller polish items (ducking + fade in/out, playlist resume across
reboots).

**Non-goal:** this is not a TRMNL/InkyPi-style cloud-rendered dashboard. Every
piece here runs on-device — `FreeInkUI` renders locally from data, nothing
here introduces a FreeInk-owned backend or a hardcoded provider — and follows
the existing `WavSource`/`WavSink` callback pattern so storage and transport
choices stay the consumer firmware's, not the SDK's.

**Naming: FreeInkAudio.** This roadmap's pieces (`AudioManager`,
`AudioPlaylist`, `Microphone`, `AudioRecorder`, `AudioTags`, `TtsClient`,
`SttClient`, `PodcastFeedClient`) are the audio counterpart to `FreeInkBook`
and `FreeInkUI` in scope-of-concern — but not in scale. `FreeInkBook` is a
whole engine because turning an EPUB into e-paper pixels requires a
from-scratch typesetting engine (UAX #14 layout, hyphenation, font fallback,
page caching); nothing about audio needs a layout/typesetting equivalent —
`AudioManager` already *is* audio's "hard part" (codec decode + I2S output),
and it's complete and comparatively small. So "FreeInkAudio" stays several
small, focused pieces rather than one `FreeInkBook`-scale engine. This doc is
the seed for a future `docs/freeink-audio.md` integration guide once the
roadmap ships, listed in `docs/README.md` the same way `freeink-book.md` and
`freeink-ui.md` are today — that promotion happens when the code ships, not
in this roadmap pass.

## SDK / FreeInkUI separation

Every piece in this doc is one or the other, never both. The test: **does it
do I/O, or hold state across frames independent of the screen?** If yes, it's
SDK. If it only turns already-resolved plain values into pixels or
interaction, it's FreeInkUI.

- **SDK** (`libs/hardware/AudioManager`, `libs/hardware/Microphone`,
  `libs/network/TtsClient`, the proposed `SttClient`/`PodcastFeedClient`):
  owns the codec/I2S, the mic, the HTTP request, and any buffering. Nothing
  in FreeInkUI may `#include` these headers or hold an `AudioManager&`,
  `AudioPlaylist&`, `AudioRecorder&`, `TtsClient&`, `SttClient&`, or
  `PodcastFeedClient&`.
- **FreeInkUI**: every existing component takes a plain `XxxProps` struct of
  values/enums plus a `Rect` — see `ProgressBarProps`
  (`libs/ui/FreeInkUI/include/components/controls/progress-bar.h`) or the
  `BatteryIndicatorStyle`/`BatteryBarFill` enums
  (`.../bars/battery-indicator.h`). The audio components below follow the
  same shape: a `NowPlayingBarProps` with a title string, a `bool isPlaying`,
  and a position/duration pair — never a reference to the SDK object
  producing those values.
- **The app (consumer firmware) does the wiring**, every frame: it holds the
  SDK instances *and* the FreeInkUI screen, reads plain values off the
  former, and passes them into the latter's props. This is one-directional
  (SDK state → UI props) — audio doesn't need an adapter class the way
  `FreeInkUIInputManager.h` bridges `InputManager` events into FreeInkUI's
  action routing, because nothing about audio feeds input *into* the UI.
- **Content acquisition follows the same boundary, one level up**: the SDK
  parses/renders *one* container (one MP3's tags, one podcast feed) the same
  way `FreeInkBook` parses one EPUB; the app walks storage and curates the
  library (chapters, tracks, episodes) into a list, the same way nothing in
  the SDK walks a directory of EPUBs either. See "Content sources for music
  and podcasts" below.

This matches how the LVGL-parity table in the main README already frames
FreeInkUI: primitives and domain components, never hardware or network
access.

## Current state (shipped)

| Piece | What it does | Location | Capability flag |
|---|---|---|---|
| `AudioManager` | WAV playback (`play`/`playBuffer`) and opt-in MP3 decode (`playMp3`) through an I2S codec | `libs/hardware/AudioManager/` | `FREEINK_CAP_AUDIO`, `FREEINK_CAP_MP3` |
| `AudioPlaylist` | Auto-advancing chapter/track queue on top of `AudioManager` | `libs/hardware/AudioManager/include/AudioPlaylist.h` | — |
| `Microphone` | PDM mic capture, raw 16-bit mono PCM | `libs/hardware/Microphone/` | `FREEINK_CAP_MIC` |
| `AudioRecorder` | Frames `Microphone` PCM as a standard WAV file into a caller-supplied sink | `libs/hardware/Microphone/include/AudioRecorder.h` | — |
| `SecureHttpClient` | Minimal streaming HTTP/1.1 client over wolfSSL TLS 1.3 | `libs/network/SecureNet/` | `FREEINK_CAP_NET_TLS13` |
| `TtsClient` | Provider-agnostic cloud text-to-speech bridge over `SecureHttpClient` | `libs/network/TtsClient/` | (uses `FREEINK_CAP_NET_TLS13`) |
| `Buzzer` | LEDC PWM tone beeper (UI clicks, alerts) | `libs/hardware/Buzzer/` | `FREEINK_CAP_BUZZER` |

See the [README capability table](../README.md#build-composition--devices--capabilities)
for the full device/flag matrix. All seven rows above are SDK-only — none has
a FreeInkUI counterpart yet; see "A dedicated FreeInkUI audio suite" below.

## Roadmap

### 1. SttClient — voice note → text

Mirrors `TtsClient`, but reversed: uploads recorded audio and returns
transcribed text.

- **Proposed location:** `libs/network/SttClient/include/SttClient.h`,
  header-only, same shape and `library.json` pattern as `TtsClient`.
- **API sketch:**
  ```cpp
  class SttClient {
   public:
    // Builds and sends one HTTP request against `http` carrying `audio`
    // (audioLen bytes) the way the caller's provider expects — raw body,
    // JSON+base64, or the caller's own multipart encoding. Returns the HTTP
    // status, or a negative value for a builder-detected failure.
    using RequestBuilder =
        std::function<int(SecureHttpClient& http, const uint8_t* audio, size_t audioLen)>;

    // Parses whatever shape the provider's response body is in and fills
    // outText. This class does no vendor-specific JSON parsing itself,
    // matching TtsClient.
    using TextExtractor =
        std::function<bool(const std::string& responseBody, std::string& outText)>;

    int transcribe(const uint8_t* audio, size_t audioLen, const RequestBuilder& builder,
                   const TextExtractor& extractText, std::string& outText);
  };
  ```
- **Pipeline glue** (documented usage, not new SDK code): `AudioRecorder`'s
  `WavSink` can target a growable PSRAM buffer (the write-side mirror of
  `TtsClient::bufferSink`) sized for a short note; once `AudioRecorder::stop()`
  finishes, `SttClient::transcribe()` uploads that buffer. All of this is
  SDK-side and produces one plain `std::string` — how that string (and the
  recording itself) reaches the screen is FreeInkUI's job, covered next.
- **v1 limitation:** raw-body/JSON-body upload only — no multipart
  form-data encoder, to keep parity with `TtsClient`'s simplicity. Called out
  here as a known gap, not silently dropped.
- **No new capability flag needed.** This is HTTP over the already-optional
  `FREEINK_CAP_NET_TLS13`/SecureNet — no new decoder or hardware dependency,
  unlike MP3's `FREEINK_CAP_MP3`.
- **Where a note attaches:** FreeInkBook's existing `(spineIndex, charStart)`
  locator (see [freeink-book.md](freeink-book.md), "Position, links,
  progress") is the natural anchor for "a note at this reading position."
  FreeInk doesn't own a note-storage format itself — consumer firmware
  persists `{locator, timestamp, transcript}` however it likes, consistent
  with the SDK's "data, not code" convention.

### 2. A dedicated FreeInkUI audio suite (`components/audio/`)

FreeInkUI already gives a cohesive interaction domain its own top-level
directory once it's more than a component or two — `keyboard/` (`key-grid.h`,
`keyboard.h`, `qwerty-keyboard.h`) is the precedent: a primitive plus
purpose-built composites, grouped together because they're one interaction
domain, not because they share a visual shape. `media/` does the same for
book/library UI (`book-card.h`, `cover-carousel.h`, `cover-grid.h`). No
written convention forces this, but it's the strongest existing pattern —
and README's own "LVGL widget parity" table already groups components by
domain across physical directories regardless of where the files live (e.g.
its "E-reader/library surfaces" row spans both `bars/` and `media/`). Audio
clears the same bar once SttClient/AudioPlaylist/AudioRecorder are
UI-visible: it earns a real suite, not two scattered primitives plus "go
assemble it yourself" guidance.

**Proposed suite**, all under `libs/ui/FreeInkUI/include/components/audio/`:

- `level-meter.h` — `levelMeter(frame, rect, LevelMeterProps)`. The one
  genuinely new primitive: a live input-level bar, `uint8_t level` (0-100)
  refreshed by the app from a rolling RMS/peak it computes over
  `Microphone`/`AudioRecorder` samples — the meter itself never touches a
  sample buffer. Modeled on `ProgressBarProps`'s track/fill `Paint` shape,
  but kept as its own component since a level meter's semantics (a live,
  jittery reading) differ from a progress fraction.
- `now-playing-bar.h` — `nowPlayingBar(frame, rect, NowPlayingBarProps)`.
  Props: `title`, `subtitle`, `isPlaying`, a `position`/`duration` pair (or a
  0..1 fraction, matching `ProgressBarProps`'s `value`/`max`), and action IDs
  for play/pause/skip/previous. Composed internally from a `progressBar`
  plus a row of `button`s, the same way `readerChrome` composes existing
  primitives rather than hand-rolling a new drawing path.
- `track-list.h` — `trackList(frame, rect, TrackListProps)`. A browsable
  queue/library list reusing `lists/list.h`'s scrolling internally. Props: an
  array of `TrackListItemProps { title, subtitle, artwork, isPlaying,
  isAvailable }` (`artwork` is a `coverPainter`-style callback, matching
  `bookCard`'s existing pattern, or nullptr; `isAvailable` is false for a
  podcast episode not yet downloaded) plus a per-row select/play action ID.
  One generic component serves audiobook chapters, music tracks, *and*
  podcast episodes — only the data differs (see the mapping table below).
- `recording-dialog.h` — `recordingDialog(frame, rect,
  RecordingDialogProps)`. A ready composite: title, elapsed time, an
  embedded `levelMeter`, record/stop/cancel action IDs; built internally on
  `overlays/popup.h`/`option-dialog.h` chrome.
- `transcript-sheet.h` — `transcriptSheet(frame, rect,
  TranscriptSheetProps)`. A ready composite wrapping `text/text-area.h`'s
  `TextArea` in `overlays/sheet.h`'s `Sheet` chrome, with Save/Discard action
  IDs, for reviewing an `SttClient` transcript before the app persists it.
- `volume-row.h` — `volumeRow(frame, rect, VolumeRowProps)`. A labeled
  volume control (current percent, mute toggle, action ID), composed from
  `controls/slider.h` or `lists/stepper-row.h`.
- Status glyph: no new component — new icons (`volume-2`, `mic`) through the
  existing `libs/assets/Icons` generator, referenced from `status-bar.h`'s
  `StatusBar`, the same way any other status-bar glyph is added.
- `MessagePanel` already supports a title/message plus an optional progress
  bar, and covers the "Transcribing…" interim state directly — no wrapper
  needed, since it needs nothing audio-specific added.

Every prop above stays a plain value/enum/callback, per the separation rule
— richer composites are not an exception to "no SDK references in
FreeInkUI."

**Content-type mapping** — this is what makes one suite cover audiobooks,
music, podcasts, *and* voice notes:

| Content type | `title` | `subtitle` | artwork | Data comes from |
|---|---|---|---|---|
| Audiobook | Chapter title | Book title / author | Book cover | FreeInkBook's existing OPF metadata + cover, already flowing through `bookCard`/`coverGrid`'s `coverPainter` pattern |
| Music | Track title | Artist — Album | Embedded album art (ID3v2 `APIC`) | The new `AudioTags` reader (below) |
| Podcast | Episode title | Show name | Episode/show artwork URL | The new `PodcastFeedClient` (below) |
| Voice note | User label or timestamp | Reading position (chapter) | *(none, or a mic glyph)* | `AudioRecorder` + FreeInkBook's `(spineIndex, charStart)` locator |

**Where these live:** `libs/ui/FreeInkUI/include/components/audio/`, a new
top-level category directory alongside `bars/`, `controls/`, `overlays/`,
`text/`, `media/`, `lists/`, `keyboard/` — following the `keyboard/`
precedent. Tests and gallery previews go alongside the existing suite
(`libs/ui/FreeInkUI/test/host/`, `libs/ui/FreeInkUI/tools/render_gallery.sh`)
— not a new test path. A new "Audio" row in README's LVGL-parity table and a
gallery section (mirroring "E-reader/library surfaces") happen once the
suite is actually built, not in this doc.

### 3. Content sources for music and podcasts

Playback itself needs nothing new — `AudioManager`/`AudioPlaylist` already
play any WAV/MP3 byte stream regardless of whether it's a book chapter, a
song, or a podcast episode. What's missing is *display metadata*: a track's
title/artist/album, and a podcast feed's episode list. Both are SDK-shaped by
the same boundary `FreeInkBook` already follows — **the SDK parses one
container; the app curates the library** — confirmed by two facts: nothing
in the SDK today scans a directory of files (not even for EPUBs — `BookCatalog`
indexes inside one already-open book, never across a folder), and
`SecureHttpClient`/`Expat.h`'s own doc comments already anticipate exactly
this kind of feed-fetching (they call out "OPDS crawl" and "OPDS feeds,
sync protocols" as design cases).

- **`AudioTags`** (`libs/hardware/AudioManager/include/AudioTags.h`,
  header-only, reuses `AudioManager::WavSource` as its byte source — the
  same storage-agnostic pattern used a third time now, after playback and
  recording):
  ```cpp
  struct AudioTags {
    std::string title;
    std::string artist;
    std::string album;
    // Embedded album art (ID3v2 APIC), located but not copied or decoded —
    // zero/empty when no art frame is present.
    size_t artworkOffset = 0;
    size_t artworkLength = 0;
    std::string artworkMimeType;  // "image/jpeg" or "image/png"
  };

  // Extracts ID3v2 TIT2/TPE1/TALB and locates an APIC frame from the front
  // of an MP3 stream. Returns false (not an error) when no ID3v2 header is
  // present, so the app falls back to a filename — the same fallback
  // FreeInkBook uses for a missing OPF title.
  bool readId3v2Tags(const AudioManager::WavSource& source, AudioTags& outTags);
  ```
  **Album art stays allocation-free and undecoded on purpose:** `AudioTags`
  only locates the `APIC` frame's bytes within the existing stream
  (`artworkOffset`/`artworkLength`/`artworkMimeType`); the caller seeks the
  same `WavSource` and decodes those bytes through FreeInkBook's *existing*
  image-decode/dithering pipeline — the one already used for EPUB cover
  images — rather than `AudioTags` growing its own JPEG/PNG decoder. Same
  "point at bytes in the stream, let something else decode them" shape as
  everything else in this doc.
- **`PodcastFeedClient`** (`libs/network/PodcastFeedClient/include/PodcastFeedClient.h`,
  same shape/`library.json` pattern as `TtsClient`/`SttClient`):
  ```cpp
  struct PodcastEpisode {
    std::string title;
    std::string enclosureUrl;      // the episode's audio file URL
    std::string pubDate;           // RFC 822, as-is; app parses/formats
    uint32_t durationSeconds = 0;  // 0 if unknown
    std::string artworkUrl;        // falls back to the feed's own artwork
  };
  struct PodcastFeed {
    std::string title;             // show name
    std::string artworkUrl;
    std::vector<PodcastEpisode> episodes;
  };

  // Fetches feedUrl over SecureHttpClient and parses it as RSS 2.0 (with
  // iTunes podcast namespace fields) via the SDK's vendored Expat parser —
  // the exact reuse case its own docs invite. Does not download episode
  // audio, manage subscriptions, or auto-refresh.
  bool fetch(const std::string& feedUrl, PodcastFeed& outFeed);
  ```
  The app fetches an episode's `enclosureUrl` itself (e.g. via
  `SecureHttpClient` to a file on SD) and plays it like any local file once
  downloaded.
- **Explicit non-goal:** no SDK-side music/podcast library index, no
  directory scanner, no subscription/download-queue management. The app
  walks storage (`SDCardManager::listFiles`/`FsFile`) and curates its own
  library list — mirroring the fact that nothing in the SDK scans a
  directory of EPUBs either. `BookCatalog`'s `.fibc` cache file is optional
  prior art an app could mirror for a music/podcast index cache, without the
  SDK itself committing to building one.

### 4. Ducking + fade in/out

`Buzzer` (PWM tone clicks) and `AudioManager` (I2S narration/audio) can run
independently today with no coordination, and `play()`/`playMp3()`/`stop()`
jump straight to full volume or cut instantly, beyond the existing
pop-avoidance silence-prime.

- **Proposed `AudioManager` additions:**
  - `void duck(bool on)` / `void setDuckedVolume(uint8_t percent)` — temporary
    output scaling, restored when duck(false) is called.
  - `void setFadeMs(uint16_t ms)` — a linear gain ramp over the first/last
    `ms` of playback, applied as a cheap Q15 multiply on samples before
    `i2s_channel_write` in `pcmTaskLoop`/`mp3TaskLoop`.
- Needs on-device listening validation before merging (fade curve, ducking
  feel) — the same caveat already noted for MP3 in the shipped work.

### 5. Playlist resume across reboots

- Track-level resume needs no new API: `AudioPlaylist::currentIndex()`
  already exists. This is a documentation gap, not a code gap — the pattern
  is: consumer persists the index, passes it back as `startIndex` to
  `begin()` on the next boot.
- Mid-track (byte-offset) resume is the one real gap: `begin()` always seeks
  to a track's `dataStart`. Proposed small addition: an optional
  `startByteOffset` parameter on `AudioPlaylist::begin()`, threaded into the
  first track's `WavSource::seek()`. Small and optional, not required for the
  track-level case above.

## Non-goals

- No FreeInk-owned cloud service or backend for TTS/STT.
- No multipart/form-data HTTP support in `SttClient` v1.
- No built-in note-storage format — consumer firmware owns persistence.
- No cross-layer includes: an SDK header (`AudioManager.h`, `AudioRecorder.h`,
  `TtsClient.h`, `SttClient.h`, `PodcastFeedClient.h`) never includes a
  FreeInkUI header, and a FreeInkUI component header never includes one of
  these — see "SDK / FreeInkUI separation" above.
- No SDK-side music/podcast library index or directory scanner — the app
  walks storage and curates its own library list.
- No podcast subscription or download-queue management — `PodcastFeedClient`
  fetches one feed on request; scheduling/persistence is the app's.
- Not a general-purpose audio-app framework — the suite covers the specific
  flows this roadmap needs (recording, transcript review, now-playing,
  browsing a queue, volume), not every conceivable audio UI.
- Not a `FreeInkBook`-scale engine — no layout/typesetting equivalent exists
  for audio, so "FreeInkAudio" stays several small, focused pieces rather
  than one big one.

(Album art extraction is explicitly *in* scope — see `AudioTags` above — not
a non-goal.)

## Open questions

- Buffer sizing for voice notes on boards without PSRAM. Cross-check: every
  device with `FREEINK_CAP_MIC` today (Sticky, Paper Mono) also has PSRAM per
  the README's `FREEINK_FB_PSRAM` notes, so this looks like a non-issue —
  worth reconfirming if a mic-capable, PSRAM-less board is ever added.
- Should ducking live on `AudioManager` directly, or as a small separate
  arbiter class coordinating `Buzzer` vs. `AudioManager`?
- How expensive is `levelMeter`'s rolling RMS/peak calculation if it runs
  every frame the recording screen is visible — needs to stay cheap since,
  per the separation rule, that math lives in app/SDK code, not in the
  component itself.
- Should podcast episode playback support streaming (piping
  `SecureHttpClient`'s streaming `GET` directly into a `WavSource`) instead
  of always downloading to SD first? Flagged as a future idea, not v1 — v1
  is download-then-play, consistent with `AudioManager` expecting a seekable
  source.
- Should `AudioManager`/`Microphone` eventually move under a new top-level
  `libs/audio/` directory (mirroring `libs/book/`) so the newer pieces
  (`AudioTags`, `PodcastFeedClient`, etc.) have one shared home? Not decided
  or actioned here — those two are already-shipped code with real referenced
  paths (this doc, `platformio.sample.ini`), so a physical move is a
  separate, deliberately breaking pass, not something to bundle into a docs
  update.

## Validation

- The `SttClient`/`TtsClient`-shaped glue is header-only and could get a
  lightweight host test with a fake HTTP stub, mirroring existing host-test
  patterns (see [testing.md](testing.md)) — worth adding once it's built, not
  required for the spec itself.
- `AudioTags` and `PodcastFeedClient` are more concretely testable than that:
  fixture-based host tests (an ID3v2-tagged MP3 fixture with an `APIC`
  frame; a sample RSS/iTunes XML fixture), mirroring `FreeInkFont`'s and
  `FreeInkBook`'s existing fixture-based host suites.
- `nowPlayingBar`, `trackList`, and the rest of the new suite get the same
  host-side render coverage as every other FreeInkUI component:
  `libs/ui/FreeInkUI/test/host/run.sh` plus an entry in the SVG gallery
  generator — no new test infrastructure.
- Ducking/fade feel and a live STT round-trip both need real hardware and a
  live provider, per testing.md's own validation-limits section — neither is
  host-testable.
