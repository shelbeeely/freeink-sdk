#pragma once

// FreeInk audio playlist — chapter/track queue on top of AudioManager.
//
// AudioManager::play() streams exactly one WavSource; this class adds the
// audiobook-mode behavior of auto-advancing through a caller-supplied
// sequence of tracks (e.g. one WavSource per EPUB chapter's narration file)
// without teaching AudioManager itself anything about playlists.
//
// Storage-agnostic like AudioManager: a track is produced on demand by a
// caller-supplied TrackProvider (index -> WavSource), so tracks can come from
// SD files, LittleFS, or PROGMEM buffers with the same API. Call update()
// from the app's loop(); it polls AudioManager::isPlaying() to notice when
// the current track drained naturally and advances to the next one.

#include <cstddef>
#include <functional>

#include "AudioManager.h"

namespace freeink {

class AudioPlaylist {
 public:
  // Produces the WavSource for track `index` (0-based). Returning a
  // WavSource with a null `read` means "track unavailable" — the playlist
  // skips it and advances instead of calling AudioManager::play() with it.
  using TrackProvider = std::function<AudioManager::WavSource(size_t index)>;

  // Starts playback at `startIndex` of a `trackCount`-track playlist served
  // by `provider`. AudioManager is not owned; it must outlive the playlist
  // (or be stop()'d before it's destroyed). Returns false if trackCount is 0
  // or every track from startIndex onward failed to start.
  bool begin(AudioManager& mgr, TrackProvider provider, size_t trackCount, size_t startIndex = 0) {
    stop();
    if (trackCount == 0) return false;
    mgr_ = &mgr;
    provider_ = std::move(provider);
    trackCount_ = trackCount;
    return startTrack(startIndex);
  }

  // Call every loop() iteration. Advances to the next track once the current
  // one finishes on its own; does nothing once the playlist has ended or
  // after stop().
  void update() {
    if (!active_ || !mgr_) return;
    if (mgr_->isPlaying()) return;
    // The current track drained (or never started, e.g. a bad file) without
    // an explicit stop()/next()/previous() call — advance on its behalf.
    startTrack(index_ + 1);
  }

  // Stops the current track (if any) and idles the playlist. begin() starts
  // fresh; update() becomes a no-op until then.
  void stop() {
    if (mgr_ && active_) mgr_->stop();
    active_ = false;
    mgr_ = nullptr;
    provider_ = nullptr;
    trackCount_ = 0;
    index_ = 0;
  }

  // Skip forward/back one track. No-ops past either end of the playlist
  // (use stop() to end it explicitly there).
  bool next() { return active_ && startTrack(index_ + 1); }
  bool previous() { return active_ && index_ > 0 && startTrack(index_ - 1); }

  bool isPlaying() const { return active_ && mgr_ && mgr_->isPlaying(); }
  size_t currentIndex() const { return index_; }
  size_t trackCount() const { return trackCount_; }

 private:
  bool startTrack(size_t index) {
    while (index < trackCount_) {
      const AudioManager::WavSource source = provider_(index);
      if (source.read && mgr_->play(source, /*loop=*/false)) {
        index_ = index;
        active_ = true;
        return true;
      }
      ++index;  // unavailable/failed track — try the next one
    }
    // Ran off the end of the playlist.
    active_ = false;
    return false;
  }

  AudioManager* mgr_ = nullptr;
  TrackProvider provider_;
  size_t trackCount_ = 0;
  size_t index_ = 0;
  bool active_ = false;
};

}  // namespace freeink

using AudioPlaylist = freeink::AudioPlaylist;
