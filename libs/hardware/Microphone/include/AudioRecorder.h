#pragma once

// FreeInk voice-note recorder — mic capture to a WAV file, on top of
// Microphone.
//
// Microphone gives raw 16-bit mono PCM (see Microphone.h); this class adds
// the voice-note behavior of framing that PCM as a standard WAV file so it's
// playable elsewhere (AudioManager::play(), a desktop player, etc.) without
// teaching Microphone anything about file formats.
//
// Storage-agnostic like AudioManager's WavSource, but in the write direction:
// a caller-supplied WavSink writes bytes wherever they belong (SD, LittleFS)
// with the same API regardless of backend. `write` is required; `seek` is
// optional — without it the 44-byte header is left with placeholder sizes
// (0xFFFFFFFF, matching the historic "streaming/unknown length" WAV
// convention), and bytesRecorded() is the only way to learn how much was
// captured. With `seek`, stop() rewrites the header's RIFF and data chunk
// sizes once the final length is known.
//
// Pull the mic every loop() by calling update() — same "call it often" model
// as AudioManager's task and Microphone's read(): a short read timeout keeps
// a single update() call from blocking the caller's frame budget.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

#include "Microphone.h"

namespace freeink {

class AudioRecorder {
 public:
  struct WavSink {
    // Appends len bytes at the sink's current write position. Return false
    // on a write failure (stops the recording).
    std::function<bool(const uint8_t* data, size_t len)> write;
    // Absolute seek from the start of the WAV, for stop()'s header patch-up.
    // Leave null if the sink can't seek (e.g. a streaming pipe); the header
    // then keeps its placeholder sizes.
    std::function<bool(size_t pos)> seek;
  };

  // Powers up the mic (via `mic`, not owned — it must outlive the recorder or
  // be stop()'d first) and writes a 44-byte PCM WAV header to `sink` before
  // any audio. Returns false if the board has no mic or the header write
  // fails.
  bool begin(Microphone& mic, const WavSink& sink, uint32_t sampleRate = Microphone::kDefaultSampleRate) {
    stop();
    if (!mic.begin(sampleRate)) return false;

    uint8_t header[kHeaderBytes];
    writeHeader(header, sampleRate, /*dataLength=*/kUnknownLength);
    if (!sink.write(header, sizeof(header))) {
      mic.end();
      return false;
    }

    mic_ = &mic;
    sink_ = sink;
    sampleRate_ = sampleRate;
    bytesRecorded_ = 0;
    recording_ = true;
    return true;
  }

  // Call every loop() iteration while recording=true. Pulls whatever PCM is
  // available (bounded by timeoutMs) and appends it to the sink. Returns
  // false if the sink rejected a write — the recording has already been
  // stopped (header patched, mic released) by the time it returns.
  bool update(uint32_t timeoutMs = 20) {
    if (!recording_) return false;
    int16_t samples[kChunkSamples];
    const int n = mic_->read(samples, kChunkSamples, timeoutMs);
    if (n <= 0) return true;  // timeout or transient read miss — not an error
    const size_t bytes = static_cast<size_t>(n) * sizeof(int16_t);
    if (!sink_.write(reinterpret_cast<const uint8_t*>(samples), bytes)) {
      stop();
      return false;
    }
    bytesRecorded_ += bytes;
    return true;
  }

  // Stops capture, releases the mic, and — if the sink supports seek —
  // rewrites the header's RIFF and data chunk sizes now that the final
  // length is known. Idempotent.
  void stop() {
    if (!recording_) return;
    if (sink_.seek) {
      uint8_t header[kHeaderBytes];
      writeHeader(header, sampleRate_, bytesRecorded_);
      if (sink_.seek(0)) sink_.write(header, sizeof(header));
    }
    if (mic_) mic_->end();
    mic_ = nullptr;
    sink_ = WavSink{};
    recording_ = false;
  }

  bool isRecording() const { return recording_; }
  size_t bytesRecorded() const { return bytesRecorded_; }
  uint32_t sampleRate() const { return sampleRate_; }

 private:
  static constexpr size_t kHeaderBytes = 44;
  static constexpr size_t kChunkSamples = 256;  // mono samples per update() pass
  static constexpr uint32_t kUnknownLength = 0xFFFFFFFFu;

  // Standard 16-bit mono PCM WAV header. dataLength == kUnknownLength writes
  // the streaming-length placeholder in both size fields.
  static void writeHeader(uint8_t* h, uint32_t sampleRate, uint32_t dataLength) {
    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBitsPerSample = 16;
    const uint32_t byteRate = sampleRate * kChannels * (kBitsPerSample / 8);
    const uint16_t blockAlign = kChannels * (kBitsPerSample / 8);
    const uint32_t riffSize = dataLength == kUnknownLength ? kUnknownLength : dataLength + 36;

    memcpy(h, "RIFF", 4);
    putLE32(h + 4, riffSize);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    putLE32(h + 16, 16);        // fmt chunk size
    putLE16(h + 20, 1);         // PCM
    putLE16(h + 22, kChannels);
    putLE32(h + 24, sampleRate);
    putLE32(h + 28, byteRate);
    putLE16(h + 32, blockAlign);
    putLE16(h + 34, kBitsPerSample);
    memcpy(h + 36, "data", 4);
    putLE32(h + 40, dataLength);
  }

  static void putLE16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
  }
  static void putLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
  }

  Microphone* mic_ = nullptr;
  WavSink sink_;
  uint32_t sampleRate_ = 0;
  size_t bytesRecorded_ = 0;
  bool recording_ = false;
};

}  // namespace freeink

using AudioRecorder = freeink::AudioRecorder;
