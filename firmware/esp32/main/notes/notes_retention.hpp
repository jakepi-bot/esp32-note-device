// The card's retention rule: what the device deletes by itself, and what it
// refuses to delete (docs/waveshare-epaper-154-v2.md §7.4).
//
// Only `sent/` is ever a candidate. A note lands there once the server has
// acknowledged it, so "synced" is the folder, not a flag that could go stale:
// a note in `pending/` (waiting, retrying) or `failed/` (rejected) is never
// deleted by this rule, whatever its age or the free space.
//
// Two reasons to delete, both oldest first:
//   - age: recorded more than `max_age_s` ago (14 days). A note whose own
//     stamp says it was recorded before the board ever had a clock is aged
//     from its note.json instead; when neither says a real time, it is not
//     deleted for age at all — an unknown age is not an old age.
//   - space: free space below `min_free_pct` (10%). Notes go until the card is
//     back above the threshold or `sent/` is empty.
//
// Plain POSIX file work on the Store, with the free-space reading passed in:
// the host tests drive it against a temp directory and a made-up card, the
// device against `/sdcard` and esp_vfs_fat_info. Deletion goes through
// Store::erase, which removes the audio before note.json (the mirror of the
// write order) so a power cut mid-delete cannot leave a note.json pointing at
// audio that is already gone.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "notes_store.hpp"

namespace hgp::notes {

// The rule, as the device applies it without being asked.
struct RetentionPolicy {
  uint32_t max_age_s = 14u * 24u * 60u * 60u;  // 14 days
  uint8_t min_free_pct = 10;                   // percent of the card's size
};

// What the mount reports about the card. `total == 0` means the answer is
// unknown (no card, or a volume that will not report): the space rule then
// stays off, and nothing is deleted for it.
struct CardSpace {
  uint64_t total = 0;
  uint64_t free = 0;
};

struct RetentionResult {
  bool evaluated = false;      // the rule ran (the caller has the tree mounted)
  uint32_t deleted = 0;        // synced notes removed by this pass
  uint64_t deleted_bytes = 0;  // their recording.wav sizes, as far as they had one
  uint32_t deleted_for_age = 0;
  uint32_t deleted_for_space = 0;
  uint32_t unsynced_left = 0;  // pending + failed notes the pass left alone
  bool clock_set = false;      // there was a real clock to age notes against
  int free_pct_before = -1;    // -1: the card did not say
  int free_pct_after = -1;     // after this pass's deletions (the card's own
                               // number again once the caller re-reads it)
  bool low_space = false;      // still below min_free_pct when the pass ended
};

// Applies the rule once. `now_s` is the device's clock (std::time(nullptr));
// `active_id` is the note in use this moment — the one recording or uploading —
// and is never deleted even if it somehow sits in sent/. Returns what it did,
// not what it would like to do: every count is a folder that is gone.
RetentionResult enforce(Store& store, const CardSpace& space, int64_t now_s, const RetentionPolicy& policy,
                        std::string_view active_id = {});

}  // namespace hgp::notes
