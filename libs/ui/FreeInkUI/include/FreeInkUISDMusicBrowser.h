#pragma once

// FreeInk SDK — SDCardManager-backed MusicBrowserLister (opt-in).
//
// Adapts SDCardManager's raw FsFile enumeration to the MusicBrowserLister
// seam music-browser.h defines, the same way FreeInkUIIcon.h adapts Icon
// bitmaps: only compilable in firmwares that also add SDCardManager to
// lib_deps. Kept out of music-browser.h so the browser's navigation state
// machine (breadcrumb stack, selection) stays freestanding and host-testable
// without a real SD card.

#include <SDCardManager.h>

#include "components/media/music-browser.h"

#include <stdlib.h>
#include <strings.h>

namespace freeink {
namespace ui {

struct SDMusicBrowserConfig {
  // Case-insensitive extensions (without the dot) counted as tracks; every
  // other file is skipped. Folders are always listed.
  const char *const *extensions = nullptr;
  uint8_t extensionCount = 0;
};

inline const SDMusicBrowserConfig &defaultSDMusicBrowserConfig() {
  static const char *const kExtensions[] = {"mp3",  "wav", "flac",
                                            "m4a",  "ogg", "aac", "wma"};
  static const SDMusicBrowserConfig kConfig{
      kExtensions, sizeof(kExtensions) / sizeof(kExtensions[0])};
  return kConfig;
}

namespace detail {

inline bool hasAudioExtension(const char *name,
                              const SDMusicBrowserConfig &config) {
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1]) return false;
  for (uint8_t i = 0; i < config.extensionCount; ++i) {
    if (strcasecmp(dot + 1, config.extensions[i]) == 0) return true;
  }
  return false;
}

inline int compareMusicEntries(const void *a, const void *b) {
  const MusicEntry *ea = static_cast<const MusicEntry *>(a);
  const MusicEntry *eb = static_cast<const MusicEntry *>(b);
  if (ea->kind != eb->kind)
    return ea->kind == MusicEntryKind::Folder ? -1 : 1;
  return strcasecmp(ea->name, eb->name);
}

}  // namespace detail

// MusicBrowserLister backed by SDCardManager. Pass a `const
// SDMusicBrowserConfig*` as userData to customize the extension allowlist,
// or nullptr to use defaultSDMusicBrowserConfig().
inline uint16_t sdMusicBrowserLister(const char *path, MusicEntry *out,
                                     uint16_t maxEntries, void *userData) {
  if (!SdMan.ready() || maxEntries == 0) return 0;
  const SDMusicBrowserConfig &config =
      userData ? *static_cast<const SDMusicBrowserConfig *>(userData)
               : defaultSDMusicBrowserConfig();

  FsFile dir = SdMan.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  uint16_t count = 0;
  for (FsFile f = dir.openNextFile(); f && count < maxEntries;
       f = dir.openNextFile()) {
    char name[64];
    f.getName(name, sizeof(name));
    if (name[0] == '.') {  // hidden/system entries (._*, .Trashes, ...)
      f.close();
      continue;
    }
    const bool isDir = f.isDirectory();
    if (!isDir && !detail::hasAudioExtension(name, config)) {
      f.close();
      continue;
    }
    MusicEntry &entry = out[count];
    entry = MusicEntry{};
    strncpy(entry.name, name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    entry.kind = isDir ? MusicEntryKind::Folder : MusicEntryKind::Track;
    entry.sizeBytes = isDir ? 0 : static_cast<uint32_t>(f.size());
    ++count;
    f.close();
  }
  dir.close();

  qsort(out, count, sizeof(MusicEntry), detail::compareMusicEntries);
  return count;
}

}  // namespace ui
}  // namespace freeink
