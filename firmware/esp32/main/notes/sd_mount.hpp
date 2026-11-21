// One mount owner for the card: `/sdcard` (FAT) is mounted once and shared by
// the SD log mirror and the offline note tree. Two esp_vfs_fat_sdmmc_mount
// calls on one mount point fail, and unmounting under an open file corrupts
// nothing but loses the file, so both users go through here.
//
// A failed mount is not cached as a failure: the caller retries on its own
// schedule (the log mirror every 30 s, the note mode when it next needs the
// card), and `format_if_needed` is only ever true for the console's explicit
// `notes format` — the firmware never formats a card by itself.
#pragma once

#include <string>

#include "sdmmc_cmd.h"

namespace hgp {

struct SdConfig;

namespace sd_mount {

// The board's slot (board.sd). Must run before the first ensure().
void configure(const SdConfig& sd);
// Mounts if needed. False with `error` set when no usable card answers; the
// next call tries again.
bool ensure(bool format_if_needed = false, std::string* error = nullptr);
// Drops the mount (a write failed: the card was pulled or the volume broke).
// The VFS refuses to unmount while a file is open, in which case the mount
// stays and the caller keeps its failure.
void release();
bool mounted();
sdmmc_card_t* card();

}  // namespace sd_mount
}  // namespace hgp
