#pragma once

// FreeInk SDK — iPod-style folder/track browser for FreeInkUI.
//
// Browsing only: this drives drill-down navigation over a directory tree
// (breadcrumb stack, selection, virtualized list) and renders with the
// existing `list()` component. It does not touch playback — pair it with
// AudioManager (or leave it purely as a file browser) as the app sees fit.
//
// Freestanding, like the rest of FreeInkUI: this header knows nothing about
// SD cards or Arduino. It walks an injected MusicBrowserLister the same way
// AudioManager takes an injected WavSource, so the navigation state machine
// is host-testable without a real SD card. The SD-backed adapter lives in
// the opt-in FreeInkUISDMusicBrowser.h (only compilable in firmwares that
// also add SDCardManager to lib_deps).

#include "../../FreeInkUICore.h"
#include "../lists/list.h"

#include <stdio.h>
#include <string.h>

namespace freeink {
namespace ui {

enum class MusicEntryKind : uint8_t { Folder, Track };

struct MusicEntry {
  char name[64] = {0};
  MusicEntryKind kind = MusicEntryKind::Track;
  uint32_t sizeBytes = 0;
};

// Lists the immediate children of `path` into `out` (capacity maxEntries),
// pre-sorted (folders before tracks, alphabetical within each group).
// Returns the count written. `path` is always an absolute path rooted at
// whatever MusicBrowser::begin() was given (e.g. "/Music", "/Music/Live").
using MusicBrowserLister = uint16_t (*)(const char *path, MusicEntry *out,
                                        uint16_t maxEntries, void *userData);

// Formats bytes as "512 B" / "48 KB" / "3.4 MB" into `out` (>= 12 bytes).
inline void formatMusicEntrySize(uint32_t bytes, char *out, size_t outLen) {
  if (outLen == 0) return;
  if (bytes < 1024) {
    snprintf(out, outLen, "%u B", static_cast<unsigned>(bytes));
  } else if (bytes < 1024u * 1024u) {
    snprintf(out, outLen, "%u KB", static_cast<unsigned>(bytes / 1024u));
  } else {
    const unsigned tenths =
        static_cast<unsigned>(bytes / (1024u * 1024u / 10u));
    snprintf(out, outLen, "%u.%u MB", tenths / 10, tenths % 10);
  }
}

// Fills `out` (capacity >= count) from browser entries: folders get a
// trailing ">" value (the drill-down affordance), tracks get their
// formatted size. ListItem::value is a borrowed pointer, so the formatted
// text needs storage that outlives the draw call — `sizeScratch` supplies
// it (e.g. `char sizeScratch[MaxEntries][12]`).
inline void musicBrowserListItems(const MusicEntry *entries, uint16_t count,
                                  ListItem *out, char sizeScratch[][12]) {
  for (uint16_t i = 0; i < count; ++i) {
    ListItem item{};
    item.label = entries[i].name;
    item.actionValue = static_cast<int16_t>(i);
    if (entries[i].kind == MusicEntryKind::Folder) {
      item.value = ">";
    } else {
      formatMusicEntrySize(entries[i].sizeBytes, sizeScratch[i], 12);
      item.value = sizeScratch[i];
    }
    out[i] = item;
  }
}

// Stateful drill-down navigator: breadcrumb stack + the current directory's
// entries + a ListNav viewport, all fixed-capacity (no heap, no std::string).
template <uint16_t MaxEntries, uint8_t MaxDepth = 8, size_t MaxPathLen = 192>
class MusicBrowser {
 public:
  void begin(MusicBrowserLister lister, void *userData,
             const char *rootPath = "/", const char *rootLabel = "Music") {
    lister_ = lister;
    userData_ = userData;
    depth_ = 0;
    copyTrunc(path_, rootPath, MaxPathLen);
    copyTrunc(rootLabel_, rootLabel, sizeof(rootLabel_));
    refresh();
  }

  // Re-lists the current directory. Call after begin() or whenever the
  // backing store changed out from under an open screen.
  void refresh() {
    count_ = lister_ ? lister_(path_, entries_, MaxEntries, userData_) : 0;
    nav_.reset(0);
  }

  // Drills into entries()[index] if it is a folder (and refreshes). Returns
  // false for a track, an out-of-range index, or a full breadcrumb stack —
  // selecting a track is left to the caller (this is a browser, not a
  // player).
  bool enter(uint16_t index) {
    if (index >= count_ || entries_[index].kind != MusicEntryKind::Folder)
      return false;
    if (depth_ >= MaxDepth) return false;
    const size_t originalLen = strlen(path_);
    const size_t nameLen = strlen(entries_[index].name);
    const bool needSlash = originalLen == 0 || path_[originalLen - 1] != '/';
    const size_t joinLen = needSlash ? 1 : 0;
    if (originalLen + joinLen + nameLen + 1 > MaxPathLen) return false;
    size_t cursor = originalLen;
    if (needSlash) path_[cursor++] = '/';
    memcpy(path_ + cursor, entries_[index].name, nameLen + 1);
    segStart_[depth_] = static_cast<uint16_t>(originalLen);
    ++depth_;
    refresh();
    return true;
  }

  // Pops one breadcrumb level and refreshes. False at the root.
  bool back() {
    if (depth_ == 0) return false;
    --depth_;
    path_[segStart_[depth_]] = '\0';
    refresh();
    return true;
  }

  bool atRoot() const { return depth_ == 0; }
  uint8_t depth() const { return depth_; }
  const char *currentPath() const { return path_; }

  // The last path segment (or the root label at depth 0) — a reasonable
  // header title for navHeader(). Callers wanting a full trail can walk
  // currentPath() themselves.
  const char *breadcrumbLabel() const {
    if (depth_ == 0) return rootLabel_;
    const char *slash = strrchr(path_, '/');
    return slash ? slash + 1 : path_;
  }

  uint16_t count() const { return count_; }
  const MusicEntry *entries() const { return entries_; }
  const MusicEntry *entry(uint16_t index) const {
    return index < count_ ? &entries_[index] : nullptr;
  }

  ListNav &nav() { return nav_; }

 private:
  static void copyTrunc(char *dst, const char *src, size_t cap) {
    if (cap == 0) return;
    strncpy(dst, src ? src : "", cap - 1);
    dst[cap - 1] = '\0';
  }

  MusicBrowserLister lister_ = nullptr;
  void *userData_ = nullptr;
  MusicEntry entries_[MaxEntries]{};
  uint16_t count_ = 0;
  char path_[MaxPathLen] = {0};
  char rootLabel_[24] = {0};
  uint16_t segStart_[MaxDepth] = {0};
  uint8_t depth_ = 0;
  ListNav nav_{};
};

}  // namespace ui
}  // namespace freeink
