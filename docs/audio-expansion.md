# Audio Expansion

## Overview

FreeInk's audio subsystem today covers WAV and (opt-in) MP3 output through
`AudioManager`, PDM microphone capture through `Microphone`, and
provider-agnostic TLS-backed bridges (`SecureNet`, `TtsClient`) for reaching a
cloud service without the SDK hardcoding any vendor. This doc is a short
near-term roadmap for what's next: a speech-to-text counterpart to
`TtsClient`, and a couple of smaller polish items that came up in the process
(audio/buzzer ducking + fade in/out, playlist resume across reboots).

**Non-goal:** this is not a TRMNL/InkyPi-style cloud-rendered dashboard. Every
piece here runs on-device — `FreeInkUI` renders locally from data, nothing
here introduces a FreeInk-owned backend or a hardcoded provider — and follows
the existing `WavSource`/`WavSink` callback pattern so storage and transport
choices stay the consumer firmware's, not the SDK's.

## SDK / FreeInkUI separation

Every piece in this doc is one or the other, never both. The test: **does it
do I/O, or hold state across frames independent of the screen?** If yes, it's
SDK. If it only turns already-resolved plain values into pixels or
interaction, it's FreeInkUI.

- **SDK** (`libs/hardware/AudioManager`, `libs/hardware/Microphone`,
  `libs/network/TtsClient`, the proposed `SttClient`): owns the codec/I2S,
  the mic, the HTTP request, and any buffering. Nothing in FreeInkUI may
  `#include` these headers or hold an `AudioManager&`, `AudioPlaylist&`,
  `AudioRecorder&`, `TtsClient&`, or `SttClient&`.
- **FreeInkUI**: every existing component takes a plain `XxxProps` struct of
  values/enums plus a `Rect` — see `ProgressBarProps`
  (`libs/ui/FreeInkUI/include/components/controls/progress-bar.h`) or the
  `BatteryIndicatorStyle`/`BatteryBarFill` enums
  (`.../bars/battery-indicator.h`). The new audio components below follow the
  same shape: a `NowPlayingBarProps` with a title string, a `bool isPlaying`,
  and a position/duration pair — never a reference to the SDK object
  producing those values.
- **The app (consumer firmware) does the wiring**, every frame: it holds the
  `AudioManager`/`AudioPlaylist`/`AudioRecorder`/`TtsClient`/`SttClient`
  instances *and* the FreeInkUI screen, reads plain values off the former,
  and passes them into the latter's props. This is one-directional (SDK
  state → UI props) — audio doesn't need an adapter class the way
  `FreeInkUIInputManager.h` bridges `InputManager` events into FreeInkUI's
  action routing, because nothing about audio feeds input *into* the UI.

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
a FreeInkUI counterpart yet; see "FreeInkUI components for audio" below.

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

### 2. FreeInkUI components for audio

Reuse existing generic components wherever they already fit; only add a new
one where audio needs a shape nothing else provides. Every prop below is a
plain value/enum/callback, per the separation rule above — no component
holds or reaches into an SDK audio class.

**New components:**
- `bars/now-playing-bar.h` — `nowPlayingBar(frame, rect, NowPlayingBarProps)`.
  Props: `title` (track/chapter name), `isPlaying`, a `position`/`duration`
  pair (or a 0..1 fraction, matching `ProgressBarProps`'s `value`/`max`), and
  action IDs for play/pause/skip/previous (semantic action routing, the same
  pattern every other interactive component uses). Composed internally from
  a `progressBar` plus a row of `button`s, the same way `readerChrome`
  composes existing primitives rather than hand-rolling a new drawing path.
- `controls/level-meter.h` — `levelMeter(frame, rect, LevelMeterProps)`.
  Props: a single `uint8_t level` (0-100), refreshed by the app from a
  rolling RMS/peak it computes over `Microphone`/`AudioRecorder` samples —
  the meter itself never touches a sample buffer. This is the one genuinely
  new primitive here; nothing existing draws a live input-level bar. Modeled
  on `ProgressBarProps`'s track/fill `Paint` shape, but kept as its own
  component since a level meter's semantics (a live, jittery reading) differ
  from a progress fraction.

**Composed from existing components (no new component needed):**
- Recording dialog: `overlays/popup.h` or `overlays/option-dialog.h` (existing)
  plus the new `levelMeter` and an elapsed-time label, for record/stop/cancel.
- "Transcribing…" interim state: `overlays/message-panel.h`'s `MessagePanel`
  already supports a title/message plus an optional progress bar — no new
  component.
- Transcript/note review: `text/text-area.h`'s `TextArea` inside an
  `overlays/sheet.h` `Sheet`, with `button`s for Save/Discard.
- Volume control: `controls/slider.h`/`slider-row.h` or `lists/stepper-row.h`
  for a discrete volume setting — both already exist.
- A speaker/mic-active glyph in `status-bar.h`'s `StatusBar`: no new
  component, just new icons (`volume-2`, `mic`) added to the consumer's icon
  manifest via the existing generator (`libs/assets/Icons/tools/gen_icons.py`),
  the same way any other status-bar glyph is added.

**Where these live:** `libs/ui/FreeInkUI/include/components/bars/now-playing-bar.h`
and `.../controls/level-meter.h`, following FreeInkUI's existing
category-by-directory layout (`bars/`, `controls/`, `overlays/`, `text/`,
`media/`, `lists/`, `keyboard/`). Tests and gallery previews go alongside the
existing suite (`libs/ui/FreeInkUI/test/host/`,
`libs/ui/FreeInkUI/tools/render_gallery.sh`) — not a new test path.

### 3. Ducking + fade in/out

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

### 4. Playlist resume across reboots

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
  `TtsClient.h`, `SttClient.h`) never includes a FreeInkUI header, and a
  FreeInkUI component header never includes one of these — see "SDK /
  FreeInkUI separation" above.

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

## Validation

- The `SttClient`/`TtsClient`-shaped glue is header-only and could get a
  lightweight host test with a fake HTTP stub, mirroring existing host-test
  patterns (see [testing.md](testing.md)) — worth adding once it's built, not
  required for the spec itself.
- `nowPlayingBar` and `levelMeter` get the same host-side render coverage as
  every other FreeInkUI component: `libs/ui/FreeInkUI/test/host/run.sh` plus
  an entry in the SVG gallery generator — no new test infrastructure.
- Ducking/fade feel and a live STT round-trip both need real hardware and a
  live provider, per testing.md's own validation-limits section — neither is
  host-testable.
