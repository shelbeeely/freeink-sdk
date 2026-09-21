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
for the full device/flag matrix.

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
  finishes, `SttClient::transcribe()` uploads that buffer. The resulting text
  displays through FreeInkUI's `TextArea`
  (`libs/ui/FreeInkUI/include/components/text/text-area.h`), with
  `MessagePanel` covering the "Transcribing…" interim state.
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

### 2. Ducking + fade in/out

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

### 3. Playlist resume across reboots

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

## Open questions

- Buffer sizing for voice notes on boards without PSRAM. Cross-check: every
  device with `FREEINK_CAP_MIC` today (Sticky, Paper Mono) also has PSRAM per
  the README's `FREEINK_FB_PSRAM` notes, so this looks like a non-issue —
  worth reconfirming if a mic-capable, PSRAM-less board is ever added.
- Should ducking live on `AudioManager` directly, or as a small separate
  arbiter class coordinating `Buzzer` vs. `AudioManager`?

## Validation

- The `SttClient`/`TtsClient`-shaped glue is header-only and could get a
  lightweight host test with a fake HTTP stub, mirroring existing host-test
  patterns (see [testing.md](testing.md)) — worth adding once it's built, not
  required for the spec itself.
- Ducking/fade feel and a live STT round-trip both need real hardware and a
  live provider, per testing.md's own validation-limits section — neither is
  host-testable.
