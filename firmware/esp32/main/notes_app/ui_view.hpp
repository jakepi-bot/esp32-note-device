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
// The bonded phones the screens can hold. The pairing table keeps three
// (CONFIG_BT_NIMBLE_MAX_BONDS); six is the panel's own row count, so even a
// build with a bigger table lists without a scroll.
inline constexpr uint8_t kMaxPeerRows = 6;

// What a list row's right margin says: mark the exception, not the rule (§6.2).
enum class NoteMark : uint8_t { None, Pending, Failed };

// Bluetooth, as the band draws it (t_3beb371a §4). The radio is up only inside
// the Phone window - the device advertises, a phone may be on it, or it is off
// and the Wi-Fi glyph keeps the slot (§4.4).
enum class BtState : uint8_t { Off, Advertising, Connected };

// A note as the screens need it: no path, no id, no file work. The two stamp
// fields are already formatted local time: the list prints them side by side,
// the note screen prints them on two lines.
struct NoteStub {
  char day[7] = {};          // "21 Nov", 6 columns
  char clock[6] = {};        // "14:02", 5 columns
  uint16_t duration_s = 0;
  NoteMark mark = NoteMark::None;
};

// A bonded phone as the screens need it: its Bluetooth identity address - which
// is what a phone's own settings show, so it is the thing a person can match -
// and a name when something knew one. The bond store keeps keys, not names, so
// `name` is empty for a phone this device never saw named, and the address is
// then the whole answer (docs/notes-ui-spec.md §11.4).
struct Peer {
  uint8_t type = 0;      // BLE address type: public or random
  uint8_t addr[6] = {};  // print order: addr[0] is the first octet shown
  char name[13] = {};    // 12 columns, the room beside an address at scale 1
};

// Everything the screens can show, cached by the caller. Plain data so the
// whole thing can be built on a stack or handed to a test.
struct Snapshot {
  bool card = true;             // Store::ensure() succeeded
  // The station's three states, as the band shows them: off, on but not
  // associated (`wifi_on`), and associated (`wifi_up`). A board on battery used
  // to show the Bluetooth rune here instead, which said nothing about the
  // network (Jake, 2026-11-22).
  bool wifi_on = false;
  bool wifi_up = false;
  bool clock_set = true;        // the clock has been set at least once
  // "14:02", the minute the clock reads (t_3beb371a §4). Empty when the clock
  // has never been set: the band's slot is then blank, not a stale time.
  char clock_text[6] = {};
  BtState bt = BtState::Off;    // the radio's window, from the port
  int8_t battery_pct = -1;      // -1 unknown
  bool external = false;        // on external power (the bolt)
  bool low_power = false;       // the caller's battery rule, ≤10 % off power
  // What a recording will hold: the setting's seconds until the runtime says
  // otherwise, then the cap the capture actually started with (the setting, the
  // contract ceiling and the card's room, whichever is smallest). It is both the
  // recording face's cap line and the point the screen stops the capture at.
  uint32_t note_max_s = 86400;
  // The bytes behind that cap (the capture's budget: free space less the reserve,
  // never past the per-note ceiling), for the face's size. 0 = unknown.
  uint64_t record_cap_bytes = 0;
  bool recording = false;
  uint32_t record_elapsed_s = 0;
  // Armed, not started: a tap made while an upload was in flight. The glass
  // draws RECORDING with the wait on it, and the counter does not move until the
  // runtime says the capture began.
  bool record_queued = false;

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

  // A one-line reason for the slot under Home's hero, already in the glass's
  // words (a capture the stop discarded for having no audio). It outranks LAST
  // and BOOT TO RECORD while it lasts and says nothing about the hero or the
  // band, so no other state's precedence moves. Empty when there is nothing to
  // explain; the caller clears it when its few seconds are up.
  char notice[18] = {};

  // Why the device cannot be emptied right now, in the runtime's words
  // ("BUSY RECORDING", "BUSY UPLOADING"), or empty when it is idle. It is the
  // busy guard's copy: a wipe or a format confirm draws it and refuses the hold,
  // so a recording or an upload cannot be formatted out from under itself.
  char busy[20] = {};

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

  // The bonded phones, in the radio's own table order. Filled by the adapter on
  // the event that opens or returns to the Unpair screen - the bond store is the
  // radio's like the card is the store's, so this is cached data, never a read
  // from the draw path.
  Peer peers[kMaxPeerRows];
  uint8_t peer_count = 0;

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
// "0:14", "12:03", and "1:40:05" past an hour - the real length, never a clamp.
// 9 columns holds "23:59:59" and its terminator. Used for the counter on the
// recording face and the position on the playing face.
void format_duration(uint32_t seconds, char out[10]);
// The same length in 5 columns, for a slot that also carries the clock
// ("14:02 · 1h40", the note screen): below an hour "M:SS", past it "1h40".
void format_duration_short(uint32_t seconds, char out[6]);
// "OF 0:14", "OF 1:40:05": the cap line of a recording or a playing note. 12
// columns holds "OF 23:59:59" and its terminator.
void format_of(uint32_t seconds, char out[13]);
// The recording face's cap line: the time left and the room behind it, from the
// capture's own budget ("OF 1:40:05 · 2.1G"). `bytes` of 0 leaves the size out,
// so a caller that knows only the seconds still gets a cap line.
void format_cap(uint32_t seconds, uint64_t bytes, char out[24]);

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
  char clock[6] = {};            // "14:02"; empty when the clock is not set
  BtState bt = BtState::Off;     // the Bluetooth window: the Phone screen only
  bool wifi_on = false;          // the station is up, associated or not
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

// A confirm screen: one question, what it is about, and (when the device is
// busy) why the hold will be refused. All three lines are copies, so the frame
// is self-contained like the rest of the snapshot.
struct ConfirmView {
  char hero[20] = {};      // "DELETE?" / "WIPE NOTES" / "FORMAT" / "UNPAIR?"
  char line1[24] = {};     // the note's stamp / "%u NOTES GO" / "THE SD CARD"
  char line2[24] = {};     // what else goes: empty for a delete
  const char* hold = nullptr; // the legend: "HOLD: delete" / "HOLD: wipe" / "HOLD: format"
  const char* why = nullptr;  // Snapshot::busy, or null when the hold is allowed
  // The body size is the default. An unpair states a 17-column address, which
  // the body size (12 px a column) would run off the page, so those two lines
  // drop to scale 1 - the size the menu's rows already use.
  uint8_t line_scale = 2;
};
bool build_confirm(const Snapshot& snap, uint16_t index, ConfirmView& out);
// The two rows that empty the device: the count for a wipe, and for a format
// exactly what is lost (every note and the card's log).
ConfirmView build_wipe_confirm(const Snapshot& snap);
ConfirmView build_format_confirm(const Snapshot& snap);

// The Unpair screen: the bonded phones, one address a row, with the name beside
// it when the table knew one. `title` and the count line are the screen's own;
// an empty table is a line, never a blank page.
struct PeerRowView {
  char addr[18] = {};   // "A4:C1:38:7F:12:9B", 17 columns
  char name[13] = {};   // empty when the radio never knew one
};
struct UnpairView {
  uint8_t count = 0;       // rows below
  uint8_t selected = 0;    // index into the table
  bool empty = false;      // the table holds nothing
  char count_line[24] = {}; // "2 PAIRED"; empty when there is nothing to count (the slot holds any %u)
  PeerRowView row[kMaxPeerRows];
};
UnpairView build_unpair(const Snapshot& snap, uint8_t selected);
// The question one phone gets: its address, the name when known, and the
// runtime's reason when the hold would be refused. False when the index is past
// the table (a phone unpaired from the console while the question was up), so the
// frame can fall back to the list rather than asking about nobody.
bool build_unpair_confirm(const Snapshot& snap, uint8_t index, ConfirmView& out);
// "A4:C1:38:7F:12:9B" from the six octets, in print order. 18 columns with the
// terminator.
void format_peer_addr(const Peer& peer, char out[18]);

struct RecordingView {
  uint32_t elapsed_s = 0;
  // The cap this note will hit, and the bytes behind it (Snapshot::note_max_s
  // and record_cap_bytes: the capture's own budget, not the setting's default).
  uint32_t cap_s = 86400;
  uint64_t cap_bytes = 0;
  bool queued = false;     // armed by a tap during an upload; nothing recorded yet
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

// The Phone screen (t_9972e2a1): the Bluetooth window a person asked for.
// `phase` is the port's answer, `refusal` the reason a window could not open -
// the screen draws that reason rather than flashing back to Home.
enum class PhonePhase : uint8_t { Advertising, Waiting, Connected, Refused, Closed };
enum class PhoneRefusal : uint8_t { None, TableFull, RadioBusy, BleOff };

// Plain input, built by the adapter from PhoneLink's answer. No strings, so the
// host tests can drive every phase, every refusal and the countdown without a
// radio.
struct PhoneStatus {
  PhonePhase phase = PhonePhase::Advertising;
  PhoneRefusal refusal = PhoneRefusal::None;
  uint32_t left_ms = 0;   // the window's countdown
  uint8_t paired = 0;     // phones in the bond store
  uint8_t slots = 0;      // bond slots (0: no Bluetooth in this build)
  bool connected = false;
};

struct PhoneView {
  char name[18] = {};      // the device's name, what the phone's chooser shows
  const char* state = nullptr;  // "WAITING", "CONNECTED", "PAIRING FULL"
  char countdown[10] = {};  // "4:59"; empty when there is no deadline left
  char table[20] = {};     // "2 OF 3 PAIRED"; empty when unknown
  const char* note = nullptr;  // "HOLDING WINDOW OPEN", "BLE OFF", or null
};
PhoneView build_phone(const Snapshot& snap, const PhoneStatus& status);

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
