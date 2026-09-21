#pragma once

// FreeInk cloud text-to-speech bridge.
//
// FreeInk stays hardware- *and* provider-agnostic: this class knows nothing
// about any specific TTS vendor's endpoint, auth scheme, or request/response
// shape (JSON body, SSML, streaming vs. buffered audio, WAV vs. MP3 output —
// every provider differs). The caller supplies a RequestBuilder that shapes
// and sends the actual HTTP request against a SecureHttpClient the way their
// chosen provider expects (a cloud TTS API, or a self-hosted engine like
// Piper behind a small HTTP wrapper); this class only wires the plumbing
// every one of those shares: own the HTTP client's lifetime, hand the
// builder the page text and an output sink, and return its status.
//
// The synthesized audio reaches the caller as raw bytes through an AudioSink
// callback (the same shape as AudioManager::WavSource::read, reversed) —
// this class does no audio decoding. Whatever the provider returns (WAV or
// MP3), the caller feeds it to the matching AudioManager entry point:
// AudioManager::play()/playBuffer() for WAV, playMp3() for MP3 (opt-in, see
// AudioManager.h). bufferSink() below covers the common case of buffering one
// utterance (e.g. one page of narration) before playback.
//
// OPT-IN, like SecureHttpClient itself: requires -DFREEINK_NET_WOLFSSL=1 for
// https endpoints (see SecureNet's docs); this header adds nothing on top of
// that, so any build already using SecureHttpClient can use TtsClient too.
//
// Usage sketch (provider-specific pieces are illustrative, not this SDK's
// concern):
//
//   freeink::TtsClient tts;
//   uint8_t audioBuf[64 * 1024];  // sized for one page of narration
//   size_t audioLen = 0;
//   const int status = tts.synthesize(pageText,
//       [&](freeink::SecureHttpClient& http, const std::string& text,
//           const freeink::TtsClient::AudioSink& sink) {
//         http.begin("https://your-tts-provider.example/v1/speak");
//         http.addHeader("Authorization", "Bearer " YOUR_API_KEY);
//         http.addHeader("Content-Type", "application/json");
//         return http.sendRequest("POST", buildJsonBody(text), sink);
//       },
//       freeink::TtsClient::bufferSink(audioBuf, sizeof(audioBuf), audioLen));
//   if (status == 200) audioManager.playBuffer(audioBuf, audioLen, false);

#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

#include <SecureHttpClient.h>

namespace freeink {

class TtsClient {
 public:
  using AudioSink = SecureHttpClient::DataCallback;

  // Builds and sends exactly one HTTP request against `http` for `text`,
  // streaming the response body to `sink` as it arrives (http.sendRequest's
  // DataCallback overload does this directly; a builder that instead wants
  // the whole body first can use http.sendRequest(method, payload) and copy
  // http.getString() into `sink` itself). Return the HTTP status, or a
  // negative value to report a failure the builder detected itself (e.g. it
  // never got to a request at all).
  using RequestBuilder = std::function<int(SecureHttpClient& http, const std::string& text, const AudioSink& sink)>;

  // Runs `builder` for `text`, owning the SecureHttpClient instance for the
  // one request. Returns whatever `builder` returns.
  int synthesize(const std::string& text, const RequestBuilder& builder, const AudioSink& sink) {
    SecureHttpClient http;
    return builder(http, text, sink);
  }

  // Convenience AudioSink that appends into a caller-owned buffer up to
  // capacity bytes (e.g. a PSRAM block sized for one page of narration),
  // reporting the bytes written through outLen. `outLen` must outlive the
  // synthesize() call the returned sink is used with (it's captured by
  // reference, updated as bytes arrive — read it once synthesize() returns).
  // If the response would overflow capacity, the sink stops the transfer
  // (returns false) after copying as much as fits, so a truncated response
  // is reported via SecureHttpClient::responseComplete()/callbackAborted()
  // rather than silently overrunning the buffer.
  static AudioSink bufferSink(uint8_t* buffer, size_t capacity, size_t& outLen) {
    outLen = 0;
    return [buffer, capacity, &outLen](const uint8_t* data, size_t len) {
      const size_t room = capacity - outLen;
      const size_t n = len < room ? len : room;
      memcpy(buffer + outLen, data, n);
      outLen += n;
      return n == len;
    };
  }
};

}  // namespace freeink

using TtsClient = freeink::TtsClient;
