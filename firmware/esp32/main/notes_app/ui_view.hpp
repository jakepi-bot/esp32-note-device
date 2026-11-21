// What the screens say, decided without a panel and without a card.
//
// Every string the glass shows is composed here, from a snapshot the caller
// took on an *event* (a recording committed, a note deleted, an upload batch
// finished, a link change, a card insert). The draw path itself never touches
// the SD card or the radio (docs/notes-ui-spec.md §15.4), so this file is the
// only place that turns facts into copy.
//
// Plain C++ with no ESP-IDF and no POSIX file work beyond the clock: the host
// tests drive it with hand-built snapshots, which is how list ordering, the
// cursor's rotation and the Home precedence chain are checked without a board.
#pragma once

#include <cstdint>
#include <string_view>

#include "notes_app/ui_keys.hpp"

namespace hgp::notesui {

// The list holds the newest notes: 128 rows is 2 KB of stubs and covers a card
// pruned at 500 only in its newest half. The menu's count and About's count
// come from the store, never from this array, so the totals stay honest.
inline constexpr uint8_t kMaxListRows = 128;
// Six rows at the two-line footer, seven at the one-line footer (§3.4).
inline constexpr uint8_t kListRows = 6;

// What a list row's right margin says: mark the exception, not the rule (§6.2).
enum class NoteMark : uint8_t { None, Pending, Failed };

// A note as the screens need it: no path, no id, no file work. The two stamp
// fields are already formatted local time: the list prints them side by side,
// the note screen prints them on two lines.
struct NoteStub {
  char day[7] = {};          // "21 Nov", 6 columns
  char clock[6] = {};        // "14:02", 5 columns
  uint16_t duration_s = 0;
  NoteMark mark = NoteMark::None;
};

// Everything the screens can show, cached by the caller. Plain data so the
// whole thing can be built on a stack or handed to a test.
struct Snapshot {
  bool card = true;             // Store::ensure() succeeded
  bool wifi_up = false;
  bool clock_set = true;        // the clock has been set at least once
  int8_t battery_pct = -1;      // -1 unknown
  bool external = false;        // on external power (the bolt)
  bool low_power = false;       // the caller's battery rule, ≤10 % off power
  uint32_t note_max_s = 300;    // the recording cap (config, never hard-coded)
  bool recording = false;
  uint32_t record_elapsed_s = 0;

  // Upload progress: what `notes::Queue` is doing, published by the app task.
  bool upload_active = false;
  bool upload_failed = false;
  uint8_t upload_done = 0;
  uint8_t upload_total = 0;

  // Playback position, published by the player's task.
  bool playing = false;
  uint32_t play_pos_s = 0;
  uint32_t play_total_s = 0;

  // The store's own counts: real totals, not what the list holds.
  uint32_t count_pending = 0;
  uint32_t count_sent = 0;
  uint32_t count_failed = 0;

  // About. Strings copied, not pointed at, so a snapshot is self-contained.
  char firmware[12] = {};
  char device[16] = {};
  uint64_t card_free_bytes = 0;

  // The stubs the caller has read from the card. Only the window the screens
  // are about to draw is ever filled (§15.4: a directory of note.json files is
  // not something the draw path may wait for), so `notes` holds indexes
  // [stub_first, stub_first + stub_rows) - six rows for the list, one for a
  // note's own screen, one for Home's LAST line.
  NoteStub notes[kMaxListRows];
  uint8_t stub_first = 0;
  uint8_t stub_rows = 0;
  // Newest first (spec §6.3). The order is the caller's: sort by note id,
  // descending, as plain strings.
  uint16_t note_count = 0;
  // True when the card holds more notes than the list keeps: the count line then
  // says "128+" rather than pretending that is the whole card (QA D7).
  bool list_truncated = false;

  uint32_t notes_total() const { return count_pending + count_sent + count_failed; }
};

// The stub for an absolute index, or nullptr when it is outside the window.
inline const NoteStub* stub_at(const Snapshot& snap, uint16_t index) {
  if (index < snap.stub_first) return nullptr;
  const uint16_t rel = static_cast<uint16_t>(index - snap.stub_first);
  return rel < snap.stub_rows ? &snap.notes[rel] : nullptr;
}

// ---------------------------------------------------------------------------
// Time, formatted from a stamp rather than from a clock

// Local calendar fields. Built by the caller (the device's port_time already
// applies the timezone to the system clock), so nothing here reads TZ.
struct Stamp {
  uint16_t year = 1970;
  uint8_t month = 1;   // 1-based
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  bool valid = false;
};

// "21 Nov" - the note screen's hero and the confirm's restatement. 6 columns.
void format_day(const Stamp& stamp, char out[7]);
// "21 Nov 14:02" - a list row. 12 columns.
void format_when(const Stamp& stamp, char out[13]);
// "14:02" - Home's LAST line. 5 columns.
void format_clock(const Stamp& stamp, char out[6]);
// "0:14", "12:03". 5 columns at most.
void format_duration(uint32_t seconds, char out[8]);
// "OF 5:00". 7 columns.
void format_of(uint32_t seconds, char out[11]);

// A note.json's `recorded_at` (UTC, "YYYY-MM-DDTHH:MM:SSZ") as local fields.
// `local` must already be localtime-of(unix), which is why it is passed in:
// this function has no clock of its own and the tests set no TZ.
Stamp stamp_from_unix(int64_t unix_s);
bool stamp_from_iso(std::string_view iso8601, Stamp& out);

const char* month_name(unsigned month);

// ---------------------------------------------------------------------------
// The screens' copy

struct HomeView {
  const char* hero = nullptr;    // a strings.hpp constant
  char band[14] = {};            // "3 TO SEND" / "ALL SENT"; empty = none
  char sub[18] = {};             // "LAST 14:02"; empty = none
  bool wifi_up = false;
  int8_t battery_pct = -1;
  bool external = false;
  uint8_t bar_pct = 0;           // 0 = no bar, else upload progress
};

// Home's precedence chain (§4.2): hero, then the band, then the one line. The
// three never restate each other.
HomeView build_home(const Snapshot& snap);

struct MenuRowView {
  const char* label = nullptr;   // menu_label(), never a copy
  char value[8] = {};            // right column; empty is deliberate
};
struct MenuView {
  uint8_t selected = 0;
  MenuRowView rows[kMenuCount];
};
MenuView build_menu(const Snapshot& snap, uint8_t selected);

// One ledger line: the date, and a mark for the exception (§6.2).
struct ListRowView {
  char when[13] = {};
  char mark = ' ';
};
struct ListView {
  uint8_t count = 0;       // rows in the snapshot
  uint8_t selected = 0;    // index into the whole list
  uint8_t first = 0;       // index of the top row shown
  uint8_t rows = 0;        // rows drawn (≤ kListRows)
  bool empty = false;
  const char* key_line = nullptr;  // "* TO SEND" / "! FAILED" / count / null
  char key_count[8] = {};          // the count when everything is sent
  ListRowView row[kListRows];
};
// `visible_first` is the whole scroll model: the bar walks down and, at the
// last row, the window rotates (§6.4).
uint8_t list_visible_first(uint16_t selected, uint16_t count);
ListView build_list(const Snapshot& snap, uint16_t selected);

struct NoteView {
  char day[7] = {};        // "21 Nov"  - scale 3
  char when[16] = {};      // "14:02 · 0:14"
  const char* state = nullptr;
  uint8_t selected = 0;    // NoteActionItem
};
bool build_note(const Snapshot& snap, uint16_t index, uint8_t selected, NoteView& out);

struct ConfirmView {
  char when[16] = {};      // the note about to go
};
bool build_confirm(const Snapshot& snap, uint16_t index, ConfirmView& out);

struct RecordingView {
  uint32_t elapsed_s = 0;
  uint32_t cap_s = 300;
};

struct PlayingView {
  char when[16] = {};
  uint32_t elapsed_s = 0;
  uint32_t total_s = 0;
  uint8_t bar_pct = 0;
};

struct PairingView {
  uint32_t passkey = 0;
  char digits[8] = {};     // "418 207"
};

struct AboutView {
  static constexpr uint8_t kRows = 5;
  const char* label[kRows] = {};
  char value[kRows][12] = {};
};

AboutView build_about(const Snapshot& snap);

// "62.9G" for About's card line: gigabytes, one decimal, clamped so the
// string always fits its slot.
void format_capacity(uint64_t bytes, char out[12]);

}  // namespace hgp::notesui
