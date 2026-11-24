// The notes UI's screens, driven without a board: the state machine, the copy,
// the list's order and marks, the refresh classes, and the two things a user
// does with a note - play it and delete it - against real files in a temp
// directory.
//
// What is not here: the panel, the codec and the radio. Those are the ports,
// covered on the desk; everything that decides *what* the glass shows is here.
#include "check.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "hg/hal.hpp"
#include "notes/notes_capture.hpp"
#include "notes/notes_queue.hpp"
#include "notes/notes_store.hpp"
#include "notes_app/battery_facts.hpp"
#include "notes_app/strings.hpp"
#include "notes_app/ui_app.hpp"
#include "notes_app/ui_cache.hpp"
#include "notes_app/ui_peers.hpp"
#include "notes_app/ui_render.hpp"
#include "notes_app/ui_state.hpp"
#include "notes_app/ui_view.hpp"
#include "notes_app/wav_player.hpp"

namespace notes = hgp::notes;
using namespace hgp::notesui;
using hgp::notes::Meta;
using hgp::notes::NoteState;
using hgp::notes::Store;

namespace {

// The stamps are local time; a fixed TZ keeps this test's strings identical on
// every host (the device gets its TZ from port_time).
struct FixedZone {
  FixedZone() {
    setenv("TZ", "UTC", 1);
    tzset();
  }
} g_fixed_zone;

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/hg-ui-XXXXXX";
    const char* made = mkdtemp(tmpl);
    path = made ? made : "/tmp/hg-ui-fallback";
  }
  ~TempDir() {
    const std::string cmd = "rm -rf '" + path + "'";
    if (std::system(cmd.c_str()) != 0) return;
  }
};

// A note on the card: the folder, a real 1 s WAV and the note.json that makes
// it exist.
std::string make_note(Store& store, const char* id, const char* recorded_at, uint32_t seconds,
                      NoteState where = NoteState::Pending) {
  std::string error;
  store.ensure();
  if (!store.create_note(id, error)) return {};
  const std::string part = store.audio_path(NoteState::Pending, id) + ".part";
  const uint32_t frames = seconds * notes::kSampleRate;
  uint8_t header[44];
  notes::wav_header(header, frames * sizeof(int16_t));
  if (FILE* f = std::fopen(part.c_str(), "wb")) {
    std::fwrite(header, 1, sizeof header, f);
    std::vector<int16_t> tone(notes::kSampleRate, 1234);  // 1 s at a time
    for (uint32_t i = 0; i < seconds; ++i) std::fwrite(tone.data(), sizeof(int16_t), tone.size(), f);
    std::fclose(f);
  }
  std::string final_audio = store.audio_path(NoteState::Pending, id);
  std::rename(part.c_str(), final_audio.c_str());

  Meta meta;
  meta.note_id = id;
  meta.device_id = "hg-test";
  meta.device_name = "test";
  meta.firmware = "0.0.0";
  meta.created_ms = 0;
  meta.recorded_at = recorded_at;
  meta.duration_s = seconds;
  meta.idempotency_key = id;
  meta.bytes = frames * sizeof(int16_t);
  if (!store.write_meta(id, meta, error)) return {};
  if (where != NoteState::Pending) store.move(id, NoteState::Pending, where);
  return id;
}

// The snapshot a screen would draw, built the way the app builds it: reload the
// cache, then fill the window the screen is about to show.
Snapshot snapshot_for(const Cache& cache, Store& store, Screen screen, uint16_t cursor) {
  Snapshot snap;
  uint8_t first = 0, rows = 1;
  if (screen == Screen::NoteList) {
    first = list_visible_first(cursor, static_cast<uint16_t>(cache.note_count()));
    rows = kListRows;
  }
  cache.fill(store, snap, first, rows);
  return snap;
}

// "AA:BB:CC:DD:EE:01" -> a Peer. The tests read like the glass.
Peer make_peer(const char* text, const char* name = nullptr) {
  Peer peer;
  unsigned octet[6] = {};
  if (std::sscanf(text, "%x:%x:%x:%x:%x:%x", &octet[0], &octet[1], &octet[2], &octet[3], &octet[4],
                  &octet[5]) == 6) {
    for (int i = 0; i < 6; ++i) peer.addr[i] = static_cast<uint8_t>(octet[i]);
  }
  if (name != nullptr) std::snprintf(peer.name, sizeof peer.name, "%s", name);
  return peer;
}

// The bond store without a radio: the table the screen lists and the deletes it
// asked for, which is how "the erase is called exactly once" is asserted.
struct FakeBonds : BondPort {
  std::vector<Peer> peers;
  int erases = 0;
  int refused = 0;  // the store says no to the next deletes
  bool answers = true;  // false: the radio is asleep and does not speak

  uint8_t list(Peer* out, uint8_t capacity, bool* answered = nullptr) override {
    if (answered != nullptr) *answered = answers;
    if (!answers) return 0;
    uint8_t n = 0;
    for (const Peer& peer : peers) {
      if (n >= capacity) break;
      out[n++] = peer;
    }
    return n;
  }
  bool erase(const Peer& peer) override {
    ++erases;
    if (refused > 0) {
      --refused;
      return false;
    }
    for (auto it = peers.begin(); it != peers.end(); ++it) {
      if (std::memcmp(it->addr, peer.addr, sizeof peer.addr) == 0) {
        peers.erase(it);
        return true;
      }
    }
    return false;
  }
};

}  // namespace

TEST("durations keep their real length: hours past 99:59, never a clamp") {
  // A note runs until the card (or 2 GiB) stops it, so nothing on the glass may
  // clamp at the old 99:59: a 90 minute note reads "1:30:00", and the cap line
  // reads the same shape instead of a "99:59" nobody believes.
  char small[10], whole[10], long_note[10];
  format_duration(14, small);
  format_duration(59u * 60u + 59u, whole);
  format_duration(90u * 60u, long_note);
  CHECK_EQ(std::string(small), std::string("0:14"));
  CHECK_EQ(std::string(whole), std::string("59:59"));
  CHECK_EQ(std::string(long_note), std::string("1:30:00"));
  // The 24 h contract ceiling is the longest the counter can ever read.
  char ceiling[10];
  format_duration(24u * 60u * 60u, ceiling);
  CHECK_EQ(std::string(ceiling), std::string("24:00:00"));
  CHECK(hg::Canvas::text_width(ceiling, grid::kHero) <= 200);  // the hero, scale 3

  // The note screen's line also carries the clock, in 14 columns at the body
  // size: a long note takes the short form rather than being cut.
  char short_note[6];
  format_duration_short(90u * 60u + 5u, short_note);
  CHECK_EQ(std::string(short_note), std::string("1h30"));
  format_duration_short(14, short_note);
  CHECK_EQ(std::string(short_note), std::string("0:14"));

  // The cap line: exact below an hour, to the minute past it, with the room the
  // capture was given beside it.
  char cap_small[24], cap_hours[24], cap_card[24];
  format_cap(300, 0, cap_small);
  format_cap(90u * 60u, 2100ull * 1024ull * 1024ull, cap_hours);
  format_cap(15u * 60u, 300ull * 1024ull * 1024ull, cap_card);
  CHECK_EQ(std::string(cap_small), std::string("OF 5:00"));
  CHECK_EQ(std::string(cap_hours), std::string("OF 1:30\xc2\xb7" "2.2G"));
  CHECK_EQ(std::string(cap_card), std::string("OF 15:00\xc2\xb7" "300M"));
  // The widest line a full card can ask for still fits the body's 176 px.
  char widest[24];
  // The largest budget a note can be given: kMaxNoteFileBytes, 12 h 25 min of
  // 48 KB/s audio (the exact seconds the capture's own budget works out to).
  format_cap(44737, 2ull * 1024ull * 1024ull * 1024ull - 64ull * 1024ull, widest);
  CHECK_EQ(std::string(widest), std::string("OF 12:25\xc2\xb7" "2.1G"));
  CHECK(hg::Canvas::text_width(widest, grid::kScale) <= 176);
}

TEST("home: the hero is the first true statement, in the spec's order") {
  Snapshot snap;
  snap.card = false;
  CHECK(std::string(build_home(snap).hero) == "NO CARD");
  snap.card = true;
  snap.low_power = true;
  CHECK(std::string(build_home(snap).hero) == "LOW POWER");
  snap.low_power = false;
  snap.upload_active = true;
  snap.upload_done = 1;
  snap.upload_total = 3;
  CHECK(std::string(build_home(snap).hero) == "UPLOADING");
  snap.upload_active = false;
  snap.upload_failed = true;
  CHECK(std::string(build_home(snap).hero) == "NO SYNC");
  snap.upload_failed = false;
  snap.count_pending = 2;
  snap.wifi_up = false;
  CHECK(std::string(build_home(snap).hero) == "OFFLINE");
  snap.wifi_up = true;
  snap.clock_set = false;
  CHECK(std::string(build_home(snap).hero) == "NO TIME");
  snap.clock_set = true;
  CHECK(std::string(build_home(snap).hero) == "READY");  // READY is the fallback
}

TEST("home: the band and the line never restate the hero") {
  Snapshot snap;
  snap.count_pending = 3;
  snap.clock_set = true;
  snap.notes[0].clock[0] = '1';
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");
  snap.stub_first = 0;
  snap.stub_rows = 1;
  snap.note_count = 3;
  const HomeView view = build_home(snap);
  CHECK(std::string(view.hero) == "OFFLINE");
  CHECK(std::string(view.band) == "3 TO SEND");
  CHECK(std::string(view.sub) == "LAST 14:02");  // not "3 TO SEND" again

  // No card: the hero says it, the band is silent, the line reassures.
  Snapshot empty_card;
  empty_card.card = false;
  const HomeView no_card = build_home(empty_card);
  CHECK(std::string(no_card.hero) == "NO CARD");
  CHECK(no_card.band[0] == '\0');
  CHECK(std::string(no_card.sub) == "NOTHING LOST");

  // An empty card with nothing queued: only this state needs a verb.
  Snapshot nothing;
  const HomeView first = build_home(nothing);
  CHECK(std::string(first.hero) == "READY");
  CHECK(std::string(first.band) == "ALL SENT");
  CHECK(std::string(first.sub) == "BOOT TO RECORD");
}

TEST("home: a notice takes the line from LAST, and every more urgent line still wins") {
  Snapshot snap;
  // The line is 14 columns, the width of BOOT TO RECORD, so it fits the body.
  CHECK(hg::Canvas::text_width(sub::kNoAudioSaved, grid::kScale) <= grid::kWidth);
  snap.count_pending = 0;
  snap.clock_set = true;
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");
  snap.stub_first = 0;
  snap.stub_rows = 1;
  snap.note_count = 1;
  std::snprintf(snap.notice, sizeof snap.notice, "%s", sub::kNoAudioSaved);
  CHECK(std::string(build_home(snap).sub) == "NO AUDIO SAVED");  // over LAST 14:02

  // The hero and the band are the notice's neighbours, not its business: a
  // discarded capture leaves both exactly as they were.
  CHECK(std::string(build_home(snap).hero) == "READY");
  CHECK(std::string(build_home(snap).band) == "ALL SENT");

  // An upload in flight is the more urgent line (it is the hero's own sentence),
  // so the notice waits rather than replacing it.
  Snapshot uploading = snap;
  uploading.upload_active = true;
  uploading.upload_done = 1;
  uploading.upload_total = 3;
  CHECK(std::string(build_home(uploading).sub) == "1 OF 3 SENT");

  // A card fault and low power are Home's own faults, ahead of any notice.
  Snapshot no_card = snap;
  no_card.card = false;
  CHECK(std::string(build_home(no_card).sub) == "NOTHING LOST");
  Snapshot low = snap;
  low.low_power = true;
  CHECK(std::string(build_home(low).sub) == "PLUG IN SOON");
}

TEST("home: the upload bar is determinate and capped") {
  Snapshot snap;
  snap.upload_active = true;
  snap.upload_done = 2;
  snap.upload_total = 3;
  CHECK(build_home(snap).bar_pct == 66);
  snap.upload_done = 9;  // cannot happen; must not draw past the track
  CHECK(build_home(snap).bar_pct == 100);
  snap.upload_active = false;
  CHECK(build_home(snap).bar_pct == 0);
}

TEST("menu: the right column carries the store's counts and the link") {
  Snapshot snap;
  snap.count_pending = 3;
  snap.count_sent = 8;
  snap.count_failed = 1;
  snap.wifi_up = true;
  const MenuView view = build_menu(snap, static_cast<uint8_t>(MenuItem::Wifi));
  CHECK(view.selected == static_cast<uint8_t>(MenuItem::Wifi));
  CHECK(std::string(view.rows[0].label) == "Notes");
  CHECK(std::string(view.rows[0].value) == "12");
  CHECK(std::string(view.rows[1].value) == "3");   // Sync now: what it would upload
  CHECK(view.rows[2].value[0] == '\0');            // Phone: deliberately blank
  CHECK(std::string(view.rows[3].value) == "on");
  CHECK(view.rows[4].value[0] == '\0');            // About
  CHECK(view.rows[5].value[0] == '\0');            // Back
  snap.wifi_up = false;
  CHECK(std::string(build_menu(snap, 0).rows[3].value) == "off");
}

TEST("list: the cursor walks six rows then rotates the window") {
  CHECK(list_visible_first(0, 12) == 0);
  CHECK(list_visible_first(5, 12) == 0);
  CHECK(list_visible_first(6, 12) == 1);
  CHECK(list_visible_first(11, 12) == 6);
  CHECK(list_visible_first(3, 12) == 0);
  CHECK(list_visible_first(0, 0) == 0);
  CHECK(list_visible_first(99, 12) == 6);  // clamped to the last row
}

TEST("list: the window the app asks for is the window it draws") {
  // The device fills only the stubs the app says the next frame reads
  // (NotesUi::load_window -> ScreenApp::window -> Cache::fill). If those two
  // ever disagree the list draws blank rows - and because the window used to be
  // computed by the device from the screen alone, that is exactly what happened
  // the moment the cursor rotated the list.
  TempDir dir;
  Store store(dir.path + "/notes");
  for (int i = 0; i < 12; ++i) {
    char id[40];
    char stamp[32];
    std::snprintf(id, sizeof id, "hg-a-202611%02dT140200-%04d", 21 - i, i + 1);
    std::snprintf(stamp, sizeof stamp, "2026-11-%02dT14:02:00Z", 21 - i);
    CHECK(!make_note(store, id, stamp, 10 + i).empty());
  }
  Cache cache;
  CHECK(cache.reload(store));
  CHECK(cache.note_count() == 12);

  // Boot the way the device does: the count is known from the cache, the stubs
  // are not read until a screen asks for them.
  Snapshot snap;
  snap.note_count = 12;
  snap.count_pending = 12;
  ScreenApp app;
  app.begin(snap);
  CHECK(app.window().first == 0);
  CHECK(app.window().rows == 1);  // Home's LAST line, nothing more

  app.press(Key::BootHold, 0);
  CHECK(app.screen() == Screen::Menu);
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::Notes) app.press(Key::BootTap, 0);
  app.press(Key::Power, 0);
  CHECK(app.screen() == Screen::NoteList);
  CHECK(app.window().rows == kListRows);

  // Walk the whole list. On every step, fill exactly the app's window and check
  // the list can draw six rows out of it - including the steps that rotate.
  for (uint16_t cursor = 0; cursor < 12; ++cursor) {
    CHECK(app.state().note == cursor);
    const Window want = app.window();
    Snapshot live = snap;
    cache.fill(store, live, want.first, want.rows);
    const ListView view = build_list(live, cursor);
    CHECK(view.first == want.first);
    CHECK(view.rows == kListRows);
    for (uint8_t row = 0; row < view.rows; ++row) {
      CHECK(view.row[row].when[0] != '\0');  // every row has its stamp
    }
    if (cursor + 1 < 12) app.press(Key::BootTap, 0);
  }

  // The note's own screen asks for exactly the note it opened.
  const Outcome open = app.press(Key::Power, 0);
  CHECK(open.effect == Effect::None);
  CHECK(app.screen() == Screen::NoteAction);
  const Window one = app.window();
  CHECK(one.first == app.open_note());
  CHECK(one.rows == 1);
  Snapshot note_snap = snap;
  cache.fill(store, note_snap, one.first, one.rows);
  NoteView note;
  CHECK(build_note(note_snap, one.first, 0, note));
  CHECK(note.day[0] != '\0');

  // Keeping the list's window instead - what the device used to do - leaves the
  // note's index unread, which is the defect this case pins.
  Snapshot list_window = snap;
  cache.fill(store, list_window, 0, kListRows);
  NoteView stale;
  CHECK(app.open_note() >= kListRows);
  CHECK(!build_note(list_window, app.open_note(), 0, stale));
}

TEST("note: a note past 100 minutes shows its real length, not a clamp") {
  // 5400 s is 90 minutes: past the old 99:59 ceiling, and the length is the one
  // fact this screen exists to show. The line also carries the clock, so the
  // long form ("14:02 \u00b7 1:30:00") is one column too wide - the short form says
  // the same length in five.
  Snapshot snap;
  std::snprintf(snap.notes[0].day, sizeof snap.notes[0].day, "21 Nov");
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");
  snap.stub_first = 0;
  snap.stub_rows = 1;
  snap.notes[0].duration_s = 5400;
  NoteView view;
  CHECK(build_note(snap, 0, 0, view));
  CHECK_EQ(std::string(view.when), std::string("14:02 \xc2\xb7 1h30"));
  CHECK(hg::Canvas::text_width(view.when, grid::kScale) <= grid::kWidth);

  // Under an hour the plain time is the clearest, and it fits beside the clock.
  snap.notes[0].duration_s = 20u * 60u;
  CHECK(build_note(snap, 0, 0, view));
  CHECK_EQ(std::string(view.when), std::string("14:02 \xc2\xb7 20:00"));

  // The counter on the note's own screen is the same number, never 99:59.
  snap.notes[0].duration_s = 5400;
  CHECK(build_note(snap, 0, 0, view));
  CHECK(std::string(view.when).find("99:59") == std::string::npos);
}

TEST("list: rows carry a date and a mark for the exception only") {
  TempDir dir;
  Store store(dir.path + "/notes");
  make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 14, NoteState::Pending);
  make_note(store, "hg-a-20261120T091100-0002", "2026-11-20T09:11:00Z", 60, NoteState::Sent);
  make_note(store, "hg-a-20261119T184000-0003", "2026-11-19T18:40:00Z", 5, NoteState::Failed);

  Cache cache;
  CHECK(cache.reload(store));
  CHECK(cache.pending() == 1);
  CHECK(cache.sent() == 1);
  CHECK(cache.failed() == 1);

  const Snapshot snap = snapshot_for(cache, store, Screen::NoteList, 0);
  const ListView view = build_list(snap, 0);
  CHECK(view.rows == 3);
  CHECK(std::string(view.row[0].when) == "21 Nov 14:02");  // newest first
  CHECK(view.row[0].mark == '*');
  CHECK(std::string(view.row[1].when) == "20 Nov 09:11");
  CHECK(view.row[1].mark == ' ');  // sent is the rule, not the exception
  CHECK(std::string(view.row[2].when) == "19 Nov 18:40");
  CHECK(view.row[2].mark == '!');
  CHECK(view.key_line != nullptr && std::string(view.key_line) == "! FAILED");  // failure wins
}

TEST("list: an all-sent card shows the count where the key would be") {
  TempDir dir;
  Store store(dir.path + "/notes");
  make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 14, NoteState::Sent);
  make_note(store, "hg-a-20261120T091100-0002", "2026-11-20T09:11:00Z", 60, NoteState::Sent);
  Cache cache;
  cache.reload(store);
  const Snapshot snap = snapshot_for(cache, store, Screen::NoteList, 0);
  const ListView view = build_list(snap, 0);
  CHECK(view.key_line == nullptr);
  CHECK(std::string(view.key_count) == "2");
  CHECK(view.row[0].mark == ' ');
  CHECK(view.row[1].mark == ' ');
}

TEST("list: an empty card says so, and advertises record rather than next") {
  TempDir dir;
  Store store(dir.path + "/notes");
  Cache cache;
  cache.reload(store);
  const Snapshot snap = snapshot_for(cache, store, Screen::NoteList, 0);
  const ListView view = build_list(snap, 0);
  CHECK(view.empty);
  CHECK(view.rows == 0);
  CHECK(view.key_line == nullptr);
  CHECK(view.key_count[0] == '\0');
}

TEST("the key map reaches every screen, and every screen has a way back") {
  Snapshot snap;
  snap.note_count = 2;
  snap.stub_first = 0;
  snap.stub_rows = 2;
  snap.count_pending = 2;
  snap.notes[0].duration_s = 14;
  std::snprintf(snap.notes[0].day, sizeof snap.notes[0].day, "21 Nov");
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");
  snap.notes[0].mark = NoteMark::Pending;
  snap.notes[1].duration_s = 60;
  std::snprintf(snap.notes[1].day, sizeof snap.notes[1].day, "20 Nov");
  std::snprintf(snap.notes[1].clock, sizeof snap.notes[1].clock, "09:11");
  snap.notes[1].mark = NoteMark::Pending;

  ScreenApp app;
  app.begin(snap);
  CHECK(app.screen() == Screen::Home);

  // Home -> Menu -> Notes -> Note -> Confirm -> back out. The walk is hand-over
  // after hand-over, so the frames are partials - three of them, then the
  // fourth, which the ghost budget spends on a full refresh (§15.2).
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);
  CHECK(app.screen() == Screen::Menu);
  // BOOT walks the rows; PWR selects. Walk to About, then run it.
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::About) {
    CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Partial);
  }
  const Outcome about = app.press(Key::Power, 0);
  CHECK(about.refresh == Refresh::Partial);  // the second hand-over
  CHECK(app.screen() == Screen::About);
  CHECK(app.press(Key::Power, 0).effect == Effect::None);  // a tap is not power, anywhere
  CHECK(app.screen() == Screen::About);  // the page stays put
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);  // hold: back to Menu
  CHECK(app.screen() == Screen::Menu);

  // Back to Notes and in.
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::Notes) app.press(Key::BootTap, 0);
  const Outcome open_list = app.press(Key::Power, 0);
  CHECK(open_list.effect == Effect::None);
  CHECK(app.screen() == Screen::NoteList);
  // The walk's fourth move, and the budget's one flash: three hand-overs of
  // residue cleared, and the pages that follow start their own run.
  CHECK(open_list.refresh == Refresh::Full);
  CHECK(app.press(Key::Power, 0).effect == Effect::None);  // opens, not plays
  CHECK(app.screen() == Screen::NoteAction);
  CHECK(app.press(Key::Power, 0).effect == Effect::StartPlayback);  // Play is row 0
  CHECK(app.screen() == Screen::Playing);

  // The player reaching the end returns to the note page.
  CHECK(app.playback_finished().effect == Effect::StopPlayback);
  CHECK(app.screen() == Screen::NoteAction);

  // Delete asks first, and the ask can be backed out of for free.
  CHECK(app.press(Key::BootTap, 0).effect == Effect::None);  // row 1: Delete
  CHECK(app.state().action == static_cast<uint8_t>(NoteActionItem::Delete));
  CHECK(app.press(Key::Power, 0).effect == Effect::None);
  CHECK(app.screen() == Screen::ConfirmDelete);
  CHECK(app.press(Key::BootTap, 0).effect == Effect::None);  // tap = keep
  CHECK(app.screen() == Screen::NoteList);
}

TEST("about: a hold goes back to the menu, a tap does nothing, and a tap is not power") {
  State s;
  s.screen = Screen::About;
  CHECK(press_boot(s) == Action::None);
  CHECK(press_power(s) == Action::None);  // PWR is not power here, or anywhere
  CHECK(hold_boot(s) == Action::Back);
  CHECK(s.screen == Screen::Menu);
}

TEST("refresh: a screen change is full, a bar move is partial, eight moves force a full") {
  // Six notes: one page, so the cursor wraps without rotating and the only
  // thing that changes is the bar - which is what the eight-move bound is for.
  Snapshot snap;
  snap.note_count = 6;
  snap.stub_first = 0;
  snap.stub_rows = 6;
  snap.count_pending = 6;
  for (uint8_t i = 0; i < 6; ++i) {
    std::snprintf(snap.notes[i].day, sizeof snap.notes[i].day, "21 Nov");
    std::snprintf(snap.notes[i].clock, sizeof snap.notes[i].clock, "1%u:0%u", i, i);
    snap.notes[i].mark = NoteMark::Pending;
  }
  ScreenApp app;
  app.begin(snap);
  app.press(Key::BootHold, 0);   // menu
  app.press(Key::Power, 0);      // Notes
  CHECK(app.screen() == Screen::NoteList);

  // Seven moves stay on the same page: partial each time.
  for (uint8_t i = 0; i < 7; ++i) {
    const Outcome out = app.press(Key::BootTap, 0);
    CHECK(out.draw);
    CHECK(out.refresh == Refresh::Partial);
  }
  // The eighth is the forced full that clears the vacated bars (§15.3).
  const Outcome eighth = app.press(Key::BootTap, 0);
  CHECK(eighth.refresh == Refresh::Full);
}

TEST("refresh: a list rotation is full however few moves it took") {
  Snapshot snap;
  snap.note_count = 20;
  snap.stub_first = 0;
  snap.stub_rows = 6;
  snap.count_pending = 20;
  for (uint8_t i = 0; i < 6; ++i) {
    std::snprintf(snap.notes[i].day, sizeof snap.notes[i].day, "21 Nov");
    std::snprintf(snap.notes[i].clock, sizeof snap.notes[i].clock, "1%u:0%u", i, i);
    snap.notes[i].mark = NoteMark::Pending;
  }
  ScreenApp app;
  app.begin(snap);
  app.press(Key::BootHold, 0);
  app.press(Key::Power, 0);
  CHECK(app.screen() == Screen::NoteList);
  // Rows 0..5 are the first page; the sixth press moves the cursor to row 6,
  // which rotates the window and repaints six rows of text at once.
  for (uint8_t i = 0; i < 5; ++i) {
    CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Partial);
  }
  CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Full);
}

TEST("refresh: a Home hero change is full, a quiet publish draws nothing") {
  Snapshot snap;
  ScreenApp app;
  app.begin(snap);
  Snapshot quiet = snap;
  const Outcome none = app.publish(quiet, DirtyNone);
  CHECK(!none.draw);
  CHECK(none.refresh == Refresh::None);

  Snapshot uploading = snap;
  uploading.upload_active = true;
  uploading.upload_done = 1;
  uploading.upload_total = 2;
  const Outcome hero = app.publish(uploading, DirtyUpload);
  CHECK(hero.draw);
  CHECK(hero.refresh == Refresh::Full);  // READY -> UPLOADING ghosts under a partial

  Snapshot done = uploading;
  done.upload_active = false;
  done.upload_done = 2;
  const Outcome ready = app.publish(done, DirtyUpload);
  CHECK(ready.refresh == Refresh::Full);  // UPLOADING -> READY, three slots at once

  Snapshot battery = done;
  battery.battery_pct = 50;
  const Outcome power = app.publish(battery, DirtyPower);
  CHECK(power.draw);
  CHECK(power.refresh == Refresh::Partial);  // the hero did not move
}

TEST("refresh: a hand-over that returns to a stale hero is full") {
  // The hero word is the one thing on Home a partial cannot draw honestly: a
  // change that lands while another page is up (an upload starting on the menu)
  // is not on the glass at all, so the frame that comes back to Home would draw
  // the new word over the old one (§4.3).
  Snapshot snap;
  snap.card = true;
  ScreenApp app;
  app.begin(snap);

  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);  // Home -> menu
  CHECK(app.screen() == Screen::Menu);

  Snapshot uploading = snap;
  uploading.upload_active = true;
  uploading.upload_done = 1;
  uploading.upload_total = 2;
  // The menu redraws, and the hero it does not show is remembered as stale.
  CHECK(app.publish(uploading, DirtyUpload).refresh == Refresh::Partial);

  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Full);  // menu -> Home
  CHECK(app.screen() == Screen::Home);

  // And once Home is drawn with the truth, the next walk is a partial again.
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);
  CHECK(app.screen() == Screen::Menu);
}

TEST("refresh: a hand-over is partial, and the fourth in a row spends the budget") {
  // The walk the person actually makes - Home -> menu -> list -> note and back -
  // is hand-over after hand-over, and each one is a partial over the rows either
  // page inked rather than the ~1.4 s full waveform (§15.2). What a partial
  // leaves behind is residue, so the machine keeps its own budget: three
  // hand-overs, then one full refresh. A session therefore carries at most three
  // pages of ghost, and the flash lands on the fourth move, not on every key.
  Snapshot snap;
  snap.card = true;
  snap.note_count = 1;
  snap.stub_first = 0;
  snap.stub_rows = 1;
  std::snprintf(snap.notes[0].day, sizeof snap.notes[0].day, "21 Nov");
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");

  ScreenApp app;
  app.begin(snap);

  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);  // Home -> menu
  CHECK(app.screen() == Screen::Menu);
  CHECK(app.press(Key::Power, 0).refresh == Refresh::Partial);     // menu -> list
  CHECK(app.screen() == Screen::NoteList);
  CHECK(app.press(Key::Power, 0).refresh == Refresh::Partial);     // list -> note
  CHECK(app.screen() == Screen::NoteAction);

  // The fourth: the budget's one flash, and the run starts over after it.
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Full);     // note -> list
  CHECK(app.screen() == Screen::NoteList);
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);  // list -> menu
  CHECK(app.screen() == Screen::Menu);
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);  // menu -> Home
  CHECK(app.screen() == Screen::Home);

  // The faces the panel must be sure of keep their full class: the recording
  // face (the counts move under it), and a pairing ask (an interrupt from the
  // radio, drawn over whatever page the person was on).
  CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Recording);
  CHECK(app.press(Key::PowerHold, 0).effect == Effect::None);  // not a shutdown
  const Outcome ask = app.pairing_ask(123456);
  CHECK(ask.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Pairing);

  // And the sleeping face, which is the last frame the panel is given.
  CHECK(app.sleep_face().refresh == Refresh::Full);
}

TEST("capture: recording again keeps the note recorded before it") {
  // start() opens with abort(), which removes the files it still holds. finish()
  // used to leave the committed note's paths behind, so every new recording
  // erased the previous note's recording.wav: a note already on the card with
  // its note.json, and - when a tap lands during a sync - the file the uploader
  // was reading. The note that is committed belongs to the card, not to this
  // object.
  TempDir dir;
  Store store(dir.path + "/notes");
  CHECK(store.ensure());
  notes::Capture capture;
  std::vector<int16_t> samples(notes::kSampleRate, 1234);  // 1 s
  std::string error;
  struct stat st {};

  Meta first;
  first.note_id = "hg-a-20261121T140200-0001";
  CHECK(capture.start(store, first.note_id, 60, error));
  capture.feed(samples.data(), samples.size());
  CHECK(capture.finish(first, error));
  const std::string first_audio = store.audio_path(NoteState::Pending, first.note_id);
  CHECK(::stat(first_audio.c_str(), &st) == 0);
  const off_t first_size = st.st_size;
  CHECK(first_size > 44);

  Meta second;
  second.note_id = "hg-a-20261121T140300-0002";
  CHECK(capture.start(store, second.note_id, 60, error));
  CHECK(::stat(first_audio.c_str(), &st) == 0);
  CHECK(st.st_size == first_size);
  capture.feed(samples.data(), samples.size());
  CHECK(capture.finish(second, error));
  CHECK(second.bytes > 0);
  CHECK(::stat(first_audio.c_str(), &st) == 0);
  CHECK(st.st_size == first_size);
}

TEST("delete: a hold on the confirm erases the note from the card") {
  TempDir dir;
  Store store(dir.path + "/notes");
  const std::string id = make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 14);
  CHECK(!id.empty());
  CHECK(store.count(NoteState::Pending) == 1);

  Cache cache;
  cache.reload(store);
  Snapshot snap = snapshot_for(cache, store, Screen::NoteList, 0);
  ScreenApp app;
  app.begin(snap);
  app.publish(snap, DirtyQueue);

  app.press(Key::BootHold, 0);  // menu
  app.press(Key::Power, 0);     // Notes
  CHECK(app.screen() == Screen::NoteList);
  app.press(Key::Power, 0);     // open the note
  CHECK(app.screen() == Screen::NoteAction);
  app.press(Key::BootTap, 0);   // Delete
  CHECK(app.press(Key::Power, 0).refresh == Refresh::Full);
  CHECK(app.screen() == Screen::ConfirmDelete);
  const Outcome del = app.press(Key::BootHold, 0);
  CHECK(del.effect == Effect::DeleteNote);
  CHECK(del.note == 0);
  CHECK(app.screen() == Screen::NoteList);

  // What the app does with the effect: erase where it lives.
  const std::string* victim = cache.id_at(del.note);
  CHECK(victim != nullptr);
  NoteState found = NoteState::Pending;
  CHECK(store.erase(*victim, &found));
  CHECK(found == NoteState::Pending);
  CHECK(store.count(NoteState::Pending) == 0);
  CHECK(access((dir.path + "/notes/pending/" + id + "/note.json").c_str(), F_OK) != 0);

  cache.reload(store);
  CHECK(cache.note_count() == 0);
  const Snapshot after = snapshot_for(cache, store, Screen::NoteList, 0);
  CHECK(build_list(after, 0).empty);
}

TEST("play: the player reads the note's own WAV, header and samples") {
  TempDir dir;
  Store store(dir.path + "/notes");
  make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 3);
  NoteState found = NoteState::Pending;
  CHECK(store.locate("hg-a-20261121T140200-0001", found));

  notes::WavReader reader;
  notes::WavInfo info;
  CHECK(reader.open(store.audio_path(found, "hg-a-20261121T140200-0001"), info));
  CHECK(info.sample_rate == notes::kSampleRate);
  CHECK(info.channels == 1);
  CHECK(info.bits_per_sample == 16);
  CHECK(info.duration_s == 3);

  int16_t samples[256];
  size_t total = 0;
  size_t frames = 0;
  while ((frames = reader.read(samples, 256)) > 0) {
    CHECK(samples[0] == 1234);  // the tone the note was written with
    total += frames;
    if (total > 3u * notes::kSampleRate) break;  // a broken header must fail fast
  }
  CHECK(total == 3u * notes::kSampleRate);
  CHECK(reader.position_s() == 3);
  CHECK(reader.read(samples, 256) == 0);  // the end stays the end
}

TEST("play: a missing or foreign file is refused, not played") {
  TempDir dir;
  notes::WavReader reader;
  notes::WavInfo info;
  CHECK(!reader.open(dir.path + "/nothing.wav", info));
  const std::string path = dir.path + "/junk.wav";
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite("not a wave file at all", 1, 22, f);
    std::fclose(f);
  }
  CHECK(!reader.open(path, info));
}

TEST("render: every screen inks paper only - x 4..196, y <= 186 at 1 bit") {
  ScreenDraw frame;
  frame.home.hero = home::kReady;
  std::snprintf(frame.home.band, sizeof frame.home.band, "3 TO SEND");
  std::snprintf(frame.home.sub, sizeof frame.home.sub, "LAST 14:02");
  frame.home.wifi_up = true;
  frame.home.battery_pct = 84;
  frame.menu = build_menu(Snapshot{}, 0);
  frame.list.count = 2;
  frame.list.rows = 2;
  std::snprintf(frame.list.row[0].when, sizeof frame.list.row[0].when, "21 Nov 14:02");
  std::snprintf(frame.list.row[1].when, sizeof frame.list.row[1].when, "20 Nov 09:11");
  frame.list.row[0].mark = '*';
  std::snprintf(frame.note.day, sizeof frame.note.day, "21 Nov");
  std::snprintf(frame.note.when, sizeof frame.note.when, "14:02 · 0:14");
  frame.note.state = note::kPending;
  std::snprintf(frame.confirm.hero, sizeof frame.confirm.hero, "%s", confirm::kHero);
  std::snprintf(frame.confirm.line1, sizeof frame.confirm.line1, "21 Nov 14:02");
  frame.confirm.hold = key::kHoldDelete;
  frame.recording.elapsed_s = 42;
  frame.recording.cap_s = 300;
  std::snprintf(frame.playing.when, sizeof frame.playing.when, "21 Nov 14:02");
  frame.playing.elapsed_s = 6;
  frame.playing.total_s = 14;
  frame.playing.bar_pct = 42;
  std::snprintf(frame.pairing.digits, sizeof frame.pairing.digits, "418 207");
  std::snprintf(frame.phone.name, sizeof frame.phone.name, "Hermes-1a2b");
  frame.phone.state = phone::kWaiting;
  std::snprintf(frame.phone.countdown, sizeof frame.phone.countdown, "4:59");
  std::snprintf(frame.phone.table, sizeof frame.phone.table, "2 OF 3 PAIRED");
  frame.phone.note = phone::kHolding;
  frame.about = build_about(Snapshot{});
  frame.busy = build_busy(false, 57, 114);  // the busiest copy a wipe has

  // The unpair list and its question, with the widest copy they can carry: a
  // 17-column address, a 12-column name beside it, and a busy line above the rule.
  Snapshot peers;
  peers.peers[0] = make_peer("AA:BB:CC:DD:EE:01", "Test Handset");
  peers.peers[1] = make_peer("AA:BB:CC:DD:EE:02", "Pixel 8 Pro");
  peers.peer_count = 2;

  static uint16_t fb[200 * 200];
  const Screen screens[] = {Screen::Home,     Screen::Menu,          Screen::NoteList,
                            Screen::NoteAction, Screen::ConfirmDelete, Screen::ConfirmWipe,
                            Screen::ConfirmFormat, Screen::Wiping,   Screen::Playing,
                            Screen::Recording, Screen::Pairing,       Screen::Phone,
                            Screen::About, Screen::UnpairList,        Screen::ConfirmUnpair};
  for (Screen screen : screens) {
    frame.screen = screen;
    if (screen == Screen::ConfirmWipe) {
      frame.confirm = build_wipe_confirm(Snapshot{});
    } else if (screen == Screen::ConfirmFormat) {
      frame.confirm = build_format_confirm(Snapshot{});
      frame.confirm.why = "BUSY RECORDING";  // the busiest copy this screen has
    } else if (screen == Screen::UnpairList) {
      frame.unpair = build_unpair(peers, 1);
    } else if (screen == Screen::ConfirmUnpair) {
      build_unpair_confirm(peers, 1, frame.confirm);
      frame.confirm.why = "BUSY RECORDING";
    }
    std::memset(fb, 0xFF, sizeof fb);  // paper
    hg::Canvas canvas(fb, 200, 200, false);
    draw(canvas, frame);

    int min_x = 200, max_x = -1, min_y = 200, max_y = -1;
    for (int y = 0; y < 200; ++y) {
      for (int x = 0; x < 200; ++x) {
        if (fb[y * 200 + x] == 0x0000) {  // ink
          if (x < min_x) min_x = x;
          if (x > max_x) max_x = x;
          if (y < min_y) min_y = y;
          if (y > max_y) max_y = y;
        }
      }
    }
    CHECK(min_x >= 4);
    CHECK(max_x <= 196);
    CHECK(max_y <= 186);
    CHECK(max_x > min_x);  // something was drawn, not a blank page
  }
}

TEST("render: nine menu rows end above the one-line footer rule") {
  // The menu grew to nine rows. At the list's 18 px pitch the last row would run
  // under the footer's rule and past the ink limit, so the menu has its own: scale
  // 1 labels at a 12 px pitch, closing above kRuleTwoLine. This is the check that
  // the column still fits when a row is added.
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  ScreenDraw frame;
  frame.screen = Screen::Menu;
  frame.menu = build_menu(Snapshot{}, static_cast<uint8_t>(kMenuCount - 1));  // Back selected
  hg::Canvas canvas(fb, 200, 200, false);
  draw(canvas, frame);

  bool inked_under_the_rows = false;
  for (int y = grid::kRowTop + grid::kMenuPitch * kMenuCount; y < grid::kRuleTwoLine; ++y) {
    for (int x = 0; x < 200; ++x) {
      if (fb[y * 200 + x] == 0x0000) inked_under_the_rows = true;
    }
  }
  CHECK(!inked_under_the_rows);

  // ...and the last row really is on the page: its selection bar starts left of
  // the margin, the way every selected row does.
  bool last_row_drawn = false;
  const int last_y = grid::kRowTop + grid::kMenuPitch * (kMenuCount - 1);
  for (int y = last_y - 1; y < last_y + grid::kMenuBar; ++y) {
    for (int x = grid::kLeft - 4; x < grid::kLeft + 4; ++x) {
      if (fb[y * 200 + x] == 0x0000) last_row_drawn = true;
    }
  }
  CHECK(last_row_drawn);
}

TEST("render: the confirm has no footer, the list marks the exception") {
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  ScreenDraw frame;
  frame.screen = Screen::ConfirmDelete;
  std::snprintf(frame.confirm.hero, sizeof frame.confirm.hero, "%s", confirm::kHero);
  std::snprintf(frame.confirm.line1, sizeof frame.confirm.line1, "21 Nov 14:02");
  frame.confirm.hold = key::kHoldDelete;
  draw(canvas, frame);
  // Nothing below the "TAP: keep" line: the rule at 162 and the footer at 172
  // belong to other screens, and this one must stay a question, not a page.
  bool inked_below_160 = false;
  for (int y = 160; y < 200; ++y) {
    for (int x = 0; x < 200; ++x) {
      if (fb[y * 200 + x] == 0x0000) inked_below_160 = true;
    }
  }
  CHECK(!inked_below_160);
}

// ---------------------------------------------------------------------------
// The Phone screen (t_9972e2a1): the menu row's own screen, the port's answer,
// the countdown's beat, every refusal, and the window ending on its own.

namespace {

// Menu -> Phone the way a person does it: a hold opens the menu, BOOT walks to
// the row, PWR selects it. Returns the effect the row produced.
Outcome open_phone_row(ScreenApp& app, uint32_t now_ms = 0) {
  for (int i = 0; i < 8 && app.screen() != Screen::Home; ++i) app.press(Key::BootHold, now_ms);
  app.press(Key::BootHold, now_ms);
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::Phone) app.press(Key::BootTap, now_ms);
  return app.press(Key::Power, now_ms);
}

PhoneStatus phone_status(PhonePhase phase, uint32_t left_ms) {
  PhoneStatus s;
  s.phase = phase;
  s.left_ms = left_ms;
  s.paired = 2;
  s.slots = 3;
  return s;
}

}  // namespace

TEST("phone: the menu row opens its own screen, not a flash back to the menu") {
  Snapshot snap;
  std::snprintf(snap.device, sizeof snap.device, "Hermes-1a2b");
  ScreenApp app;
  app.begin(snap);

  const Outcome open = open_phone_row(app);
  CHECK(open.effect == Effect::OpenPhoneWindow);
  CHECK(open.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Phone);
  const ScreenDraw first = app.frame();
  CHECK(first.screen == Screen::Phone);
  // Nothing from the port yet: the screen says ADVERTISING and no table.
  CHECK(std::string(first.phone.name) == "Hermes-1a2b");
  CHECK(std::string(first.phone.state) == phone::kAdvertising);
  CHECK(first.phone.countdown[0] == '\0');
}

TEST("phone: the port's answer draws the countdown and the pairing table") {
  Snapshot snap;
  std::snprintf(snap.device, sizeof snap.device, "Hermes-1a2b");
  ScreenApp app;
  app.begin(snap);
  open_phone_row(app);

  const Outcome waiting = app.set_phone(phone_status(PhonePhase::Waiting, 295000), 1000);
  CHECK(waiting.draw);
  const PhoneView view = app.frame().phone;
  CHECK(std::string(view.state) == phone::kWaiting);
  CHECK(std::string(view.countdown) == "4:55");
  CHECK(std::string(view.table) == "2 OF 3 PAIRED");

  // A phone connects: the word changes, the clock goes away (the window is
  // held open), and both are one full frame.
  PhoneStatus connected = phone_status(PhonePhase::Connected, 60000);
  connected.connected = true;
  const Outcome on_link = app.set_phone(connected, 2000);
  CHECK(on_link.draw);
  CHECK(on_link.refresh == Refresh::Full);
  CHECK(std::string(app.frame().phone.state) == phone::kConnected);
  CHECK(app.frame().phone.countdown[0] == '\0');
  CHECK(std::string(app.frame().phone.note) == phone::kHolding);
}

TEST("phone: the countdown repaints on the five second beat, not at 1 Hz") {
  ScreenApp app;
  app.begin(Snapshot{});
  open_phone_row(app);
  PhoneStatus st = phone_status(PhonePhase::Waiting, 296000);  // 4:56
  CHECK(app.set_phone(st, 0).draw);  // the first answer draws the clock
  st.left_ms = 294000;               // 4:54: the beat moved
  CHECK(app.set_phone(st, 1000).draw);
  st.left_ms = 292000;  // within the same beat
  CHECK(!app.set_phone(st, 2000).draw);
  st.left_ms = 290000;
  CHECK(!app.set_phone(st, 3000).draw);
  st.left_ms = 289000;  // 4:49: one beat on
  const Outcome moved = app.set_phone(st, 4000);
  CHECK(moved.draw);
  CHECK(moved.refresh == Refresh::Partial);
}

TEST("phone: every refusal is a word on the glass, with no clock") {
  const PhoneRefusal refusals[] = {PhoneRefusal::TableFull, PhoneRefusal::RadioBusy, PhoneRefusal::BleOff};
  const char* words[] = {phone::kTableFull, phone::kBusy, phone::kBleOff};
  for (int i = 0; i < 3; ++i) {
    ScreenApp app;
    app.begin(Snapshot{});
    open_phone_row(app);
    PhoneStatus st;
    st.phase = PhonePhase::Refused;
    st.refusal = refusals[i];
    const Outcome out = app.set_phone(st, 0);
    CHECK(out.draw);
    CHECK(out.refresh == Refresh::Full);
    const PhoneView view = app.frame().phone;
    CHECK(std::string(view.state) == words[i]);
    CHECK(view.countdown[0] == '\0');
    CHECK(view.note == nullptr);
    // The screen is still the row's own: no flash back to the menu.
    CHECK(app.screen() == Screen::Phone);
  }
}

TEST("phone: the window closes by itself and the screen says so") {
  ScreenApp app;
  app.begin(Snapshot{});
  open_phone_row(app);
  CHECK(app.set_phone(phone_status(PhonePhase::Waiting, 1000), 0).draw);
  const Outcome closed = app.set_phone(phone_status(PhonePhase::Closed, 0), 1000);
  CHECK(closed.draw);
  CHECK(closed.refresh == Refresh::Full);  // the word changed
  CHECK(std::string(app.frame().phone.state) == phone::kClosed);
  CHECK(std::string(app.frame().phone.note) == phone::kOff);
  CHECK(app.frame().phone.countdown[0] == '\0');
  CHECK(app.screen() == Screen::Phone);  // it stays on the glass, honestly closed
}

TEST("phone: closing the screen ends the window and returns to the menu") {
  ScreenApp app;
  app.begin(Snapshot{});
  open_phone_row(app);
  app.set_phone(phone_status(PhonePhase::Waiting, 200000), 0);

  const Outcome by_power = app.press(Key::Power, 1000);
  CHECK(by_power.effect == Effect::ClosePhoneWindow);
  CHECK(by_power.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Menu);

  // The row is still selected: PWR opens it again, and a hold closes the window
  // the same way a PWR tap does.
  CHECK(app.state().menu == static_cast<uint8_t>(MenuItem::Phone));
  CHECK(app.press(Key::Power, 1500).effect == Effect::OpenPhoneWindow);
  CHECK(app.screen() == Screen::Phone);
  const Outcome by_hold = app.press(Key::BootHold, 2000);
  CHECK(by_hold.effect == Effect::ClosePhoneWindow);
  CHECK(app.screen() == Screen::Menu);
}

// ---------------------------------------------------------------------------
// Zone A (t_3beb371a §4): the clock and the Bluetooth mark share the band with
// the queue count, the Wi-Fi glyph and the battery, and the last three do not
// move a pixel to make room.

namespace {

uint64_t ink_hash(const uint16_t* fb, int x0, int y0, int x1, int y1) {
  uint64_t h = 1469598103934665603ull;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      h ^= (fb[y * 200 + x] & 1u);
      h *= 1099511628211ull;
    }
  }
  return h;
}

// Home as the glass gets it, from a snapshot.
uint64_t home_band_hash(const Snapshot& snap, int x0, int y0, int x1, int y1) {
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  ScreenDraw frame;
  frame.screen = Screen::Home;
  frame.home = build_home(snap);
  draw(canvas, frame);
  return ink_hash(fb, x0, y0, x1, y1);
}

Snapshot band_seed() {
  Snapshot snap;
  snap.card = true;
  snap.wifi_up = true;
  snap.battery_pct = 84;
  snap.count_pending = 3;
  return snap;
}

}  // namespace

TEST("band: the clock takes its own slot and the glyphs keep theirs") {
  Snapshot with_clock = band_seed();
  with_clock.clock_set = true;
  std::snprintf(with_clock.clock_text, sizeof with_clock.clock_text, "14:02");
  Snapshot without = band_seed();
  without.clock_set = false;

  // The Wi-Fi box and the battery are byte-identical with and without a clock:
  // this is the \"the band does not shift\" rule, as ink.
  CHECK(home_band_hash(with_clock, 136, 8, 189, 24) == home_band_hash(without, 136, 8, 189, 24));
  // And the left slot changed: the time is drawn.
  CHECK(home_band_hash(with_clock, 12, 8, 133, 24) != home_band_hash(without, 12, 8, 133, 24));

  // The time is left of the glyph box, not under it: no ink in the 2 px gutter
  // at 134..135, and ink in the clock's own columns.
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  ScreenDraw frame;
  frame.screen = Screen::Home;
  frame.home = build_home(with_clock);
  draw(canvas, frame);
  for (int y = 8; y < 24; ++y) {
    for (int x = 132; x < 136; ++x) CHECK(fb[y * 200 + x] != 0x0000);
  }
  CHECK(ink_hash(fb, 96, 8, 132, 24) != home_band_hash(without, 96, 8, 132, 24));

  // A snapshot from a first frame that never got the clock says nothing rather
  // than 00:00.
  Snapshot fresh;
  CHECK(build_home(fresh).clock[0] == '\0');
  CHECK(std::string(build_home(with_clock).clock) == "14:02");
}

TEST("pairing: a second ask does not trap the screen on the question") {
  // The bench hit this with a real phone: an ask arrived, was answered, and a
  // second ask landed before the screen left, so after_pairing became Pairing
  // and no key could get off it (2026-11-22).
  ScreenApp app;
  Snapshot snap = band_seed();
  app.begin(snap);
  app.pairing_ask(0);
  CHECK(app.screen() == Screen::Pairing);
  app.pairing_ask(0);                       // a second ask, same screen
  app.pairing_answer(true);
  CHECK(app.screen() == Screen::Home);      // not Pairing again
  CHECK(app.screen() != Screen::Pairing);
}

TEST("pairing: a completed pair leaves the ask by itself") {
  // The bench's stuck ask (t_da500270): the answer was given on the console
  // (`phone allow`), so no key ever reached the screen, and the port's "phone
  // paired" left PAIRING over a phone the console already counted. The bond is
  // the answer, so the glass leaves the question without a key, and the frame
  // that replaces it is a full one (the ask owns the whole page).
  ScreenApp app;
  Snapshot snap = band_seed();
  app.begin(snap);
  app.pairing_ask(761273);
  CHECK(app.screen() == Screen::Pairing);
  const Outcome done = app.pairing_paired();
  CHECK(app.screen() == Screen::Home);
  CHECK(done.draw);
  CHECK(done.refresh == Refresh::Full);
  // A second report of the same bond is not a frame: the question is gone.
  CHECK(!app.pairing_paired().draw);
}

TEST("pairing: a completed pair lands back where the ask interrupted") {
  ScreenApp app;
  Snapshot snap = band_seed();
  app.begin(snap);
  app.press(Key::BootHold, 0);              // the menu
  CHECK(app.screen() == Screen::Menu);
  app.pairing_ask(123456);
  CHECK(app.screen() == Screen::Pairing);
  app.pairing_paired();
  CHECK(app.screen() == Screen::Menu);      // the screen the ask covered
}

TEST("pairing: a second phone's ask survives the first pair completing") {
  // Two asks can overlap: the first was answered from the console, a second
  // phone asked, and only then did the first phone's bond land. That bond is not
  // an answer to the second phone's question, so the question stays - and its
  // own bond is what closes it.
  const uint8_t first[6] = {0x0A, 0x62, 0x21, 0x20, 0xA9, 0xD3};
  const uint8_t second[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  ScreenApp app;
  Snapshot snap = band_seed();
  app.begin(snap);

  app.pairing_ask(111111, first);
  CHECK(app.screen() == Screen::Pairing);
  app.pairing_answer(true);                 // the console's allow, the screen's way out
  CHECK(app.screen() == Screen::Home);

  app.pairing_ask(222222, second);          // the phone that asked while the first paired
  CHECK(app.screen() == Screen::Pairing);
  const Outcome wrong = app.pairing_paired(first);
  CHECK(!wrong.draw);
  CHECK(app.screen() == Screen::Pairing);   // still the second phone's question

  const Outcome right = app.pairing_paired(second);
  CHECK(right.draw);
  CHECK(right.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Home);
}

TEST("pairing: a bond never closes a screen the person opened") {
  // The Phone screen is a person's own window, not a question: the port counts
  // its bond and the screen keeps drawing the table it was opened for.
  const uint8_t peer[6] = {0x0A, 0x62, 0x21, 0x20, 0xA9, 0xD3};
  ScreenApp app;
  Snapshot snap = band_seed();
  app.begin(snap);
  open_phone_row(app);                      // the Phone row: the window a person asked for
  CHECK(app.screen() == Screen::Phone);
  const Outcome done = app.pairing_paired(peer);
  CHECK(!done.draw);
  CHECK(app.screen() == Screen::Phone);
}

TEST("band: the Wi-Fi box shows the station's three states, and never Bluetooth") {
  // Off, on-but-not-associated, associated: three pictures, so a person can tell
  // "the radio is off" from "it is trying" without a console (Jake, 2026-11-22).
  Snapshot off = band_seed();
  off.wifi_up = false;                 // the seed is associated; this one is not
  off.wifi_on = false;
  Snapshot on = band_seed();
  on.wifi_up = false;
  on.wifi_on = true;                   // up, not associated
  Snapshot up = band_seed();
  up.wifi_on = true;
  up.wifi_up = true;

  const uint64_t off_box = home_band_hash(off, 136, 8, 153, 24);
  const uint64_t on_box = home_band_hash(on, 136, 8, 153, 24);
  const uint64_t up_box = home_band_hash(up, 136, 8, 153, 24);
  CHECK(off_box != on_box);
  CHECK(off_box != up_box);
  CHECK(on_box != up_box);

  // Bluetooth never takes this box: the phone window has its own screen, and a
  // board on battery used to show the advertising rune here instead of the
  // network's state, which is what made the Wi-Fi mark "disappear".
  Snapshot bluetooth_busy = off;
  bluetooth_busy.bt = BtState::Connected;
  CHECK(home_band_hash(bluetooth_busy, 136, 8, 153, 24) == off_box);
  Snapshot bluetooth_adv = up;
  bluetooth_adv.bt = BtState::Advertising;
  CHECK(home_band_hash(bluetooth_adv, 136, 8, 153, 24) == up_box);

  // Nothing bleeds out of the box into the 8 px gap the battery starts after.
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  ScreenDraw frame;
  frame.screen = Screen::Home;
  frame.home = build_home(up);
  draw(canvas, frame);
  for (int y = 8; y < 24; ++y) {
    for (int x = 152; x < 160; ++x) CHECK(fb[y * 200 + x] != 0x0000);
  }
}

// ---------------------------------------------------------------------------
// The band's battery (t_c1eab9f8): this board's port fills battery_mv and never
// battery_percent, so the UI's copy of the percentage was -1 in every poll while
// `notes status` printed 90 %. Both readers now call hgp::battery_percent
// (notes_app/battery_facts.hpp), so the level and the console cannot disagree.

namespace {

// An hg::Power that answers one canned status, the way the board's latch does.
class StubPower : public hg::Power {
 public:
  explicit StubPower(hg::PowerStatus status) : status_(status) {}
  std::optional<hg::PowerStatus> read() override { return status_; }
  bool power_off() override { return true; }

 private:
  hg::PowerStatus status_;
};

hg::PowerStatus mv_only(uint16_t mv) {
  hg::PowerStatus s;
  s.battery_mv = mv;
  s.external_power = false;  // off the cable: the level, not the bolt
  return s;
}

}  // namespace

TEST("battery: a port that answers only mV gives the console's percentage") {
  // The desk unit's own reading: `notes status` says 90 % at 4068 mV, this board
  // having no curve. The band must land on that number from the same port.
  StubPower power(mv_only(4068));
  const auto status = power.read();
  CHECK(status.has_value());
  CHECK(hgp::battery_percent(*status, hg::Curve{}) == 90);

  // The linear fallback's ends, unchanged from what the console printed before
  // the conversion was shared: 3.30 V empty, 4.15 V full.
  CHECK(hgp::battery_percent(mv_only(3300), hg::Curve{}) == 0);
  CHECK(hgp::battery_percent(mv_only(3200), hg::Curve{}) == 0);
  CHECK(hgp::battery_percent(mv_only(4150), hg::Curve{}) == 100);
  CHECK(hgp::battery_percent(mv_only(4250), hg::Curve{}) == 100);

  // A board with a measured curve uses it instead: 3704 mV is 49 % on the
  // AMOLED-1.75C's curve where the straight line says 47.
  CHECK(hgp::battery_percent(mv_only(3704), hg::kAmoled175cCurve) == 49);
  CHECK(hgp::battery_percent(mv_only(3704), hg::Curve{}) == 47);

  // A port's own percent wins over both.
  hg::PowerStatus gauge = mv_only(3704);
  gauge.battery_percent = 87;
  CHECK(hgp::battery_percent(gauge, hg::kAmoled175cCurve) == 87);
}

TEST("battery: no reading is -1, never a level") {
  // The latch with no calibration: it still answers external power and nothing
  // about the cell. That is "unknown", not 0 %.
  hg::PowerStatus external_only;
  external_only.external_power = true;
  CHECK(hgp::battery_percent(external_only, hg::Curve{}) == -1);
  CHECK(hgp::battery_percent(hg::PowerStatus{}, hg::kAmoled175cCurve) == -1);

  // And a failed read is not a reading: Power::read's contract is nullopt.
  class FailingPower : public hg::Power {
   public:
    std::optional<hg::PowerStatus> read() override { return std::nullopt; }
    bool power_off() override { return true; }
  } failing;
  CHECK(!failing.read().has_value());
}

TEST("band: a level is drawn from the reading, '?' only without one") {
  // A real percentage fills the shell; an unknown level draws '?' (a blank box
  // would read as 0 %); a cable draws the bolt. All three differ as ink.
  Snapshot level = band_seed();  // 84 %, off the cable
  level.external = false;
  Snapshot unknown = band_seed();
  unknown.battery_pct = -1;  // what every poll carried before this fix
  unknown.external = false;
  Snapshot zero = band_seed();
  zero.battery_pct = 0;
  zero.external = false;
  Snapshot cable = level;
  cable.external = true;

  const uint64_t at_84 = home_band_hash(level, 156, 8, 190, 25);
  const uint64_t at_unknown = home_band_hash(unknown, 156, 8, 190, 25);
  const uint64_t at_zero = home_band_hash(zero, 156, 8, 190, 25);
  const uint64_t on_cable = home_band_hash(cable, 156, 8, 190, 25);
  CHECK(at_84 != at_unknown);    // the level reaches the glass
  CHECK(at_unknown != at_zero);  // '?' is not a 0 % bar
  CHECK(on_cable != at_84);      // the bolt is its own glyph
  CHECK(on_cable != at_unknown);

  // The port's own answer is what fills the box: an mv-only stub at 4068 mV is
  // 90 %, and 90 % is not the unknown glyph.
  StubPower power(mv_only(4068));
  const auto status = power.read();
  CHECK(status.has_value());
  Snapshot from_port = band_seed();
  from_port.battery_pct = static_cast<int8_t>(hgp::battery_percent(*status, hg::Curve{}));
  from_port.external = false;
  CHECK(from_port.battery_pct == 90);
  CHECK(home_band_hash(from_port, 156, 8, 190, 25) != at_unknown);
}

// ---------------------------------------------------------------------------
// PWR in the player (t_dec6f9ef). The unit powered off mid-playback: from Home,
// hold -> menu, PWR -> the list, PWR -> the player, PWR again logged "power off
// asked" and the board went down. Two keys now separate: a tap is never a
// power-off on any screen, and only a hold from Home is.

namespace {

// A one-note card, with the stub the list and the player read.
Snapshot one_note_card() {
  Snapshot snap;
  snap.card = true;
  snap.note_count = 1;
  snap.stub_first = 0;
  snap.stub_rows = 1;
  snap.count_pending = 1;
  snap.notes[0].duration_s = 14;
  snap.notes[0].mark = NoteMark::Pending;
  std::snprintf(snap.notes[0].day, sizeof snap.notes[0].day, "21 Nov");
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");
  return snap;
}

// Home -> menu -> list -> note -> player, the way a finger does it: a hold opens
// the menu, BOOT walks to the Notes row, PWR selects it, opens the note and runs
// Play (row 0).
Outcome enter_player(ScreenApp& app) {
  app.press(Key::BootHold, 0);
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::Notes) app.press(Key::BootTap, 0);
  app.press(Key::Power, 0);         // Notes -> the list
  app.press(Key::Power, 0);         // the list -> the note
  return app.press(Key::Power, 0);  // the note -> Play
}

}  // namespace

TEST("player: a PWR tap stops playback and stays on, it never powers off") {
  ScreenApp app;
  app.begin(one_note_card());
  const Outcome play = enter_player(app);
  CHECK(play.effect == Effect::StartPlayback);
  CHECK(app.screen() == Screen::Playing);
  CHECK(app.snapshot().playing);

  // The tap the unit died on.
  const Outcome tap = app.press(Key::Power, 0);
  CHECK(tap.effect == Effect::StopPlayback);
  CHECK(tap.effect != Effect::PowerOff);
  CHECK(!app.snapshot().playing);
  CHECK(app.screen() == Screen::NoteList);  // back where the player was opened
  CHECK(tap.refresh == Refresh::Full);
  CHECK(tap.draw);
}

TEST("keys: only a PWR hold from Home reaches PowerOff") {
  // The whole screen table, as ui_keys sees it: no tap is a power-off, and no
  // hold outside Home is one either.
  const Screen screens[] = {Screen::Home,     Screen::Recording,    Screen::Menu,
                            Screen::NoteList, Screen::NoteAction,   Screen::ConfirmDelete,
                            Screen::Playing,  Screen::Pairing,      Screen::Phone,
                            Screen::About,    Screen::Back};
  for (Screen screen : screens) {
    State s;
    s.screen = screen;
    s.note_count = 1;
    s.playing = true;
    CHECK(press_power(s) != Action::PowerOff);
    if (screen != Screen::Home) CHECK(hold_power(s) != Action::PowerOff);
  }
  State home;
  CHECK(hold_power(home) == Action::PowerOff);

  // And through the machine, where the action becomes the effect the runtime
  // takes: one hold on Home is one PowerOff, with nothing to draw.
  ScreenApp app;
  app.begin(one_note_card());
  const Outcome first = app.press(Key::PowerHold, 0);
  CHECK(first.effect == Effect::PowerOff);
  CHECK(first.effect != Effect::StopPlayback);
  CHECK(!first.draw);
  const Outcome again = app.press(Key::PowerHold, 0);
  CHECK(again.effect == Effect::PowerOff);  // one per hold, never two

  // A tap on Home is not one; a hold in the player is not one.
  CHECK(app.press(Key::Power, 0).effect == Effect::None);
  CHECK(app.screen() == Screen::Home);
  ScreenApp playing;
  playing.begin(one_note_card());
  enter_player(playing);
  CHECK(playing.press(Key::PowerHold, 0).effect == Effect::None);
  CHECK(playing.screen() == Screen::Playing);
  CHECK(playing.snapshot().playing);  // a hold does not stop a playback either
}

TEST("keys: two PWR taps in the player leave the device running") {
  // The exact sequence that shut the unit down, asserted on the effects: the
  // screen alone cannot tell "stopped" from "switched off".
  ScreenApp app;
  app.begin(one_note_card());
  CHECK(enter_player(app).effect == Effect::StartPlayback);
  CHECK(app.screen() == Screen::Playing);

  const Outcome first = app.press(Key::Power, 0);
  const Outcome second = app.press(Key::Power, 0);
  CHECK(first.effect == Effect::StopPlayback);
  CHECK(second.effect == Effect::None);  // the list's top note opens: no effect
  CHECK(first.effect != Effect::PowerOff);
  CHECK(second.effect != Effect::PowerOff);
  CHECK(first.refresh == Refresh::Full);
  CHECK(app.snapshot().playing == false);
  CHECK(app.screen() == Screen::NoteAction);  // the second tap opened it again

  // Still answering keys. The machine has no "off" state to land in, so the
  // proof is a live screen that keeps taking presses.
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Partial);  // a hand-over
  CHECK(app.screen() == Screen::NoteList);
}

// ---------------------------------------------------------------------------
// The sleeping face (t_41477fd1): an off device must never look like an on one.
// The e-paper keeps whatever frame it was holding when the rail drops, so the
// PWR hold's last image is the only thing that says "this is off".

namespace {

// Ink in a box of the panel, and where it lands. The framebuffer is 200x200
// with ink at 0x0000 and paper at 0xFFFF, the way every screen case here reads
// it.
struct InkBox {
  int ink = 0;
  int min_x = 200, max_x = -1, min_y = 200, max_y = -1;
};

InkBox ink_in(const uint16_t* fb, int x0, int y0, int x1, int y1) {
  InkBox box;
  for (int y = y0; y < y1 && y < 200; ++y) {
    for (int x = x0; x < x1 && x < 200; ++x) {
      if (fb[y * 200 + x] != 0x0000) continue;
      ++box.ink;
      if (x < box.min_x) box.min_x = x;
      if (x > box.max_x) box.max_x = x;
      if (y < box.min_y) box.min_y = y;
      if (y > box.max_y) box.max_y = y;
    }
  }
  return box;
}

// The draw_sleep() the device calls, with the moon where it is drawn: the rune
// is 32x32 and the face puts it at (54, 40). Named here so the test and the
// renderer cannot drift apart silently (a move is a failing case, not a photo
// nobody takes).
constexpr int kMoonX = 54;
constexpr int kMoonY = 40;
constexpr int kMoonSize = 32;

}  // namespace

TEST("sleep: the frame is the sleeping face, and the panel is cleared") {
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  // A stale Home frame first: the face is drawn over whatever the person was
  // looking at when they held PWR, so every pixel of that page has to go.
  ScreenDraw home;
  home.screen = Screen::Home;
  home.home.hero = home::kReady;
  std::snprintf(home.home.band, sizeof home.home.band, "3 TO SEND");
  std::snprintf(home.home.sub, sizeof home.home.sub, "LAST 14:02");
  home.home.battery_pct = 84;
  draw(canvas, home);
  const InkBox page = ink_in(fb, 0, 0, 200, 200);
  CHECK(page.ink > 2000);  // a full page of UI text and marks was on the glass

  draw_sleep(canvas);

  // A sleeping face is a drawing on paper, not a page: under a third of what
  // Home inks (a run of text and rows is thousands of pixels), and nothing at
  // all where Home's band, hint and footer were.
  // Bigger type on the sleeping face costs ink: 18 px "hold PWR" plus a body-size
  // "2 seconds" is still a drawing, not a page, but the ceiling moves with it.
  constexpr int kClearedInk = 2000;
  const InkBox all = ink_in(fb, 0, 0, 200, 200);
  CHECK(all.ink > 400);   // the moon, the zzz and the line are there
  CHECK(all.ink < kClearedInk);
  CHECK(all.ink < page.ink / 2);
  CHECK(ink_in(fb, 0, 0, 200, 20).ink == 0);     // Home's band is gone
  CHECK(ink_in(fb, 0, 150, 200, 200).ink == 0);  // and so is its footer
  CHECK(all.min_x >= 4);   // inside the panel's own margins, as every face is
  CHECK(all.max_x <= 196);
  CHECK(all.max_y <= 186);

  // The glyph where the face puts it: the 32 px crescent, then the zzz climbing
  // out of its opening.
  const InkBox moon = ink_in(fb, kMoonX, kMoonY, kMoonX + kMoonSize, kMoonY + kMoonSize);
  CHECK(moon.ink > 200);
  CHECK(moon.max_y < kMoonY + kMoonSize);
  const InkBox zzz = ink_in(fb, kMoonX + kMoonSize, 20, 190, 60);
  CHECK(zzz.ink > 100);

  // The line, centred at the body size where draw_centred puts it, between the
  // motif and the bottom of the page. It was a 6 px caption and unreadable, so
  // the assertion is on the size a person can read, not just on its position.
  const InkBox line = ink_in(fb, 0, 92, 200, 118);
  CHECK(line.ink > 200);
  const int want_x = (200 - hg::Canvas::text_width(sleep::kHint, 3)) / 2;
  CHECK(line.min_x >= want_x);
  CHECK(line.min_x <= want_x + 2);
  CHECK(line.max_x <= want_x + hg::Canvas::text_width(sleep::kHint, 3));
  CHECK(line.min_y >= 92);
  CHECK(line.max_y <= 116);
  // And the duration under it, at the body size, still inside the page.
  const InkBox under = ink_in(fb, 0, 118, 200, 145);
  CHECK(under.ink > 60);
  CHECK(under.max_x <= 196);
}

TEST("sleep: the face draws with a full refresh") {
  // The frame the PWR hold puts on the panel is the screen machine's answer,
  // like every other frame: full, because a partial waveform over the page it
  // replaces ghosts that page through the moon. NotesUi::sleep_face() asks this
  // same ScreenApp (state_.app().sleep_face()) and hands the class to the
  // driver's request_full_refresh(), so this is what the device draws.
  ScreenApp app;
  app.begin(one_note_card());
  const Outcome out = app.sleep_face();
  CHECK(out.refresh == Refresh::Full);
  CHECK(out.draw);
  CHECK(out.effect == Effect::None);    // nothing for the runtime, only the glass
  CHECK(app.screen() == Screen::Home);  // not a screen: the machine does not move

  // And the hold that leads to it asks for no frame of its own, so the sleeping
  // face is the only one the power-off path puts on the panel.
  const Outcome hold = app.press(Key::PowerHold, 0);
  CHECK(hold.effect == Effect::PowerOff);
  CHECK(!hold.draw);
}

// ---------------------------------------------------------------------------
// The menu's two destructive rows (Jake, 2026-11-22): wipe every note and format
// the card, behind the delete confirm's key map, with the busy guard.

namespace {

// Menu -> a row's confirm, the way a person does it: a hold opens the menu, a tap
// walks to the row, PWR selects it.
Outcome open_menu_row(ScreenApp& app, MenuItem row) {
  app.press(Key::BootHold, 0);
  // The row can be behind the cursor: walk forward until it is under it.
  while (static_cast<MenuItem>(app.state().menu) != row) {
    if (app.state().menu >= kMenuCount) break;
    app.press(Key::BootTap, 0);
  }
  return app.press(Key::Power, 0);
}

}  // namespace

TEST("menu: rows in order, Back last") {
  // Nine rows: the six the menu had, the two that empty the device, and the row
  // the unpair task fills in - every one of them before Back, so one hold still
  // always leaves the menu.
  CHECK(kMenuCount == 9);
  const char* expected[kMenuCount] = {"Notes",       "Sync now", "Phone",       "Wi-Fi", "About",
                                      "Wipe notes",  "Format card", "Unpair phones", "Back"};
  for (uint8_t i = 0; i < kMenuCount; ++i) {
    CHECK(std::string(menu_label(static_cast<MenuItem>(i))) == expected[i]);
    for (uint8_t j = static_cast<uint8_t>(i + 1); j < kMenuCount; ++j) {
      CHECK(std::string(menu_label(static_cast<MenuItem>(i))) != menu_label(static_cast<MenuItem>(j)));
    }
  }
  CHECK(static_cast<MenuItem>(kMenuCount - 1) == MenuItem::Back);
  CHECK(std::string(menu_label(MenuItem::Count)).empty());  // the tally is not a row

  // The count the wipe confirm states is the store's own total.
  Snapshot snap;
  snap.card = true;
  snap.count_pending = 100;
  snap.count_sent = 12;
  snap.count_failed = 1;
  const ConfirmView wipe = build_wipe_confirm(snap);
  CHECK(std::string(wipe.hero) == "WIPE NOTES");
  CHECK(std::string(wipe.line1) == "113 NOTES GO");
  CHECK(std::string(wipe.line2) == "THE CARD STAYS");
  CHECK(wipe.hold != nullptr && std::string(wipe.hold) == "HOLD: wipe");
  CHECK(wipe.why == nullptr);  // idle: the hold is allowed
  // The hero has to fit the panel at scale 3 and the lines at the body size: the
  // wide copy on a 200 px screen is what the ink box below proves.
  static uint16_t fb[200 * 200];
  ScreenDraw frame;
  frame.screen = Screen::ConfirmWipe;
  frame.confirm = wipe;
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  draw(canvas, frame);
  int min_x = 200, max_x = -1;
  for (int y = 0; y < 200; ++y) {
    for (int x = 0; x < 200; ++x) {
      if (fb[y * 200 + x] != 0x0000) continue;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
    }
  }
  CHECK(min_x >= 4);
  CHECK(max_x <= 196);
}

TEST("confirm: a BOOT tap cancels wipe and a BOOT hold wipes") {
  Snapshot snap;
  snap.card = true;
  snap.note_count = 2;
  snap.stub_first = 0;
  snap.stub_rows = 2;
  snap.count_pending = 2;
  std::snprintf(snap.notes[0].day, sizeof snap.notes[0].day, "21 Nov");
  std::snprintf(snap.notes[0].clock, sizeof snap.notes[0].clock, "14:02");

  ScreenApp app;
  app.begin(snap);
  const Outcome ask = open_menu_row(app, MenuItem::WipeNotes);
  CHECK(ask.refresh == Refresh::Partial);  // the question is a hand-over too
  CHECK(app.screen() == Screen::ConfirmWipe);

  // The frame is the question: the row's confirm, with the count on it.
  const ScreenDraw first = app.frame();
  CHECK(first.screen == Screen::ConfirmWipe);
  CHECK(std::string(first.confirm.line1) == "2 NOTES GO");

  // A tap is the safe answer, and it is the same one PWR gives: the menu, the
  // notes untouched, nothing outside the glass to undo.
  const Outcome tap = app.press(Key::BootTap, 0);
  CHECK(tap.effect == Effect::None);
  CHECK(tap.effect != Effect::WipeNotes);
  CHECK(app.screen() == Screen::Menu);
  CHECK(app.snapshot().note_count == 2);

  CHECK(app.press(Key::Power, 0).effect == Effect::None);  // the row again
  CHECK(app.screen() == Screen::ConfirmWipe);
  const Outcome pwr = app.press(Key::Power, 0);
  CHECK(pwr.effect == Effect::None);
  CHECK(app.screen() == Screen::Menu);

  // The hold is the wipe, and the glass lands on the busy face - the frame the
  // device paints before it starts the runtime's work (t_0070a731). Home, with
  // the count the job leaves, is the frame card_job_finished() asks for.
  app.press(Key::Power, 0);
  CHECK(app.screen() == Screen::ConfirmWipe);
  const Outcome wipe = app.press(Key::BootHold, 0);
  CHECK(wipe.effect == Effect::WipeNotes);
  CHECK(wipe.refresh == Refresh::Full);
  CHECK(wipe.draw);
  CHECK(app.screen() == Screen::Wiping);
}

TEST("confirm: a BOOT tap cancels format and a BOOT hold formats") {
  Snapshot snap;
  snap.card = true;
  snap.note_count = 2;
  snap.count_pending = 2;

  ScreenApp app;
  app.begin(snap);
  CHECK(open_menu_row(app, MenuItem::FormatCard).refresh == Refresh::Partial);
  CHECK(app.screen() == Screen::ConfirmFormat);

  // A format says what it takes: the card, and the notes and log with it.
  const ScreenDraw first = app.frame();
  CHECK(first.screen == Screen::ConfirmFormat);
  CHECK(std::string(first.confirm.hero) == "FORMAT");
  CHECK(std::string(first.confirm.line1) == "THE SD CARD");
  CHECK(std::string(first.confirm.line2) == "NOTES AND LOG GO");
  CHECK(std::string(first.confirm.hold) == "HOLD: format");

  const Outcome tap = app.press(Key::BootTap, 0);
  CHECK(tap.effect == Effect::None);
  CHECK(app.screen() == Screen::Menu);

  app.press(Key::Power, 0);
  const Outcome pwr = app.press(Key::Power, 0);  // PWR cancels too
  CHECK(pwr.effect == Effect::None);
  CHECK(app.screen() == Screen::Menu);

  app.press(Key::Power, 0);
  const Outcome format = app.press(Key::BootHold, 0);
  CHECK(format.effect == Effect::FormatCard);
  CHECK(format.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Wiping);
}

TEST("guard: wipe and format are refused while the device is busy") {
  // A recording is what a format must never run over. The screen says so once,
  // from the runtime's own words, and the hold does nothing.
  Snapshot snap;
  snap.card = true;
  snap.note_count = 3;
  snap.count_pending = 3;
  std::snprintf(snap.busy, sizeof snap.busy, "BUSY RECORDING");

  ScreenApp app;
  app.begin(snap);
  open_menu_row(app, MenuItem::WipeNotes);
  CHECK(app.screen() == Screen::ConfirmWipe);
  const Outcome held = app.press(Key::BootHold, 0);
  CHECK(held.effect == Effect::None);
  CHECK(held.effect != Effect::WipeNotes);
  CHECK(app.screen() == Screen::ConfirmWipe);  // the ask stays up
  CHECK(!held.draw);                           // the reason is already on it
  const ScreenDraw frame = app.frame();
  CHECK(frame.screen == Screen::ConfirmWipe);
  CHECK(frame.confirm.why != nullptr);
  CHECK(std::string(frame.confirm.why) == "BUSY RECORDING");
  CHECK(app.snapshot().note_count == 3);  // nothing was emptied

  // The upload case, on the format confirm.
  Snapshot uploading = snap;
  std::snprintf(uploading.busy, sizeof uploading.busy, "BUSY UPLOADING");
  ScreenApp other;
  other.begin(uploading);
  open_menu_row(other, MenuItem::FormatCard);
  CHECK(other.screen() == Screen::ConfirmFormat);
  const Outcome refused = other.press(Key::BootHold, 0);
  CHECK(refused.effect == Effect::None);
  CHECK(other.screen() == Screen::ConfirmFormat);
  CHECK(std::string(other.frame().confirm.why) == "BUSY UPLOADING");

  // The same confirm, once the runtime is idle: the hold goes through.
  Snapshot idle = snap;
  idle.busy[0] = '\0';
  ScreenApp free;
  free.begin(idle);
  open_menu_row(free, MenuItem::WipeNotes);
  CHECK(free.press(Key::BootHold, 0).effect == Effect::WipeNotes);
}

TEST("unpair: the list names every bonded phone, and says so when there are none") {
  // One phone whose name the radio knew and one known only by its address. The
  // address is always there - it is the one thing a person can compare with the
  // phone in their hand - and the name is the second column when it exists.
  Snapshot snap;
  snap.peers[0] = make_peer("AA:BB:CC:DD:EE:01", "Test Handset");
  snap.peers[1] = make_peer("AA:BB:CC:DD:EE:02");
  snap.peer_count = 2;

  ScreenApp app;
  app.begin(snap);
  CHECK(open_menu_row(app, MenuItem::UnpairPhones).refresh == Refresh::Partial);
  CHECK(app.screen() == Screen::UnpairList);

  const ScreenDraw frame = app.frame();
  CHECK(frame.screen == Screen::UnpairList);
  CHECK(!frame.unpair.empty);
  CHECK(frame.unpair.count == 2);
  CHECK(std::string(frame.unpair.row[0].addr) == "AA:BB:CC:DD:EE:01");
  CHECK(std::string(frame.unpair.row[0].name) == "Test Handset");
  CHECK(std::string(frame.unpair.row[1].addr) == "AA:BB:CC:DD:EE:02");
  CHECK(frame.unpair.row[1].name[0] == '\0');
  CHECK(std::string(frame.unpair.count_line) == "2 PAIRED");

  // BOOT walks the rows, the way the note list does.
  CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Partial);
  CHECK(app.frame().unpair.selected == 1);
  CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Partial);
  CHECK(app.frame().unpair.selected == 0);  // two rows wrap

  // The empty table is its own page: a line saying so, the hint where a phone is
  // paired, and no "next" in the footer - a list that advertises a row it does
  // not have is a lie the user tests once.
  Snapshot none;
  ScreenApp empty;
  empty.begin(none);
  CHECK(open_menu_row(empty, MenuItem::UnpairPhones).refresh == Refresh::Partial);
  CHECK(empty.screen() == Screen::UnpairList);
  const ScreenDraw blank = empty.frame();
  CHECK(blank.unpair.empty);
  CHECK(blank.unpair.count == 0);
  CHECK(std::string(unpair::kEmpty) == "NOTHING PAIRED");

  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  draw(canvas, blank);
  int ink_mid = 0, ink_rows = 0;
  for (int y = 0; y < 200; ++y) {
    for (int x = 0; x < 200; ++x) {
      if (fb[y * 200 + x] != 0x0000) continue;
      if (y >= 60 && y <= 100) ++ink_mid;   // the empty line and its hint
      if (y >= 32 && y < 60) ++ink_rows;    // where rows would be
    }
  }
  CHECK(ink_mid > 0);   // the line is on the glass
  CHECK(ink_rows == 0); // and nothing pretends to be a row

  // A tap on an empty table asks about nobody and stays; the hold is the way out.
  CHECK(empty.press(Key::Power, 0).effect == Effect::None);
  CHECK(empty.screen() == Screen::UnpairList);
  CHECK(empty.press(Key::BootHold, 0).refresh == Refresh::Partial);
  CHECK(empty.screen() == Screen::Menu);
}

TEST("unpair: entering the screen reads the table then, not at boot") {
  // The radio rests until somebody asks for it: its store answers nothing at
  // boot, and by the time the row is selected it holds a phone (the device wakes
  // the radio on the way in for exactly this reason). The screen must draw what
  // the store says now - a table cached from boot would tell a person looking at
  // the phone in their hand that nothing is paired.
  TempDir dir;
  Store store(dir.path + "/notes");
  CHECK(store.ensure());
  FakeBonds bonds;  // the radio is asleep: no table yet

  UiState ui;
  Snapshot seed;
  seed.card = true;
  ui.begin(seed, &store, &bonds);
  ui.press(Key::BootHold, 0);  // Home -> menu
  while (static_cast<MenuItem>(ui.app().state().menu) != MenuItem::UnpairPhones) {
    ui.press(Key::BootTap, 0);
  }
  bonds.peers.push_back(make_peer("AA:BB:CC:DD:EE:01"));  // the radio came up

  ui.press(Key::Power, 0);  // select the row
  CHECK(ui.screen() == Screen::UnpairList);
  CHECK(ui.snapshot().peer_count == 1);
  const ScreenDraw list = ui.app().frame();
  CHECK(list.unpair.count == 1);
  CHECK(std::string(list.unpair.row[0].addr) == "AA:BB:CC:DD:EE:01");
  CHECK(bonds.erases == 0);
}

TEST("unpair: a store that does not answer leaves the table on the glass") {
  // The radio can be asleep when the screen is already up (its window closed
  // under the question). The read then comes back with nothing to say, which is
  // not the same as an empty table: the list must keep the phones it named rather
  // than telling a person their device holds none.
  TempDir dir;
  Store store(dir.path + "/notes");
  CHECK(store.ensure());
  FakeBonds bonds;
  bonds.peers.push_back(make_peer("AA:BB:CC:DD:EE:01"));
  bonds.peers.push_back(make_peer("AA:BB:CC:DD:EE:02"));

  UiState ui;
  Snapshot seed;
  seed.card = true;
  ui.begin(seed, &store, &bonds);
  ui.press(Key::BootHold, 0);
  while (static_cast<MenuItem>(ui.app().state().menu) != MenuItem::UnpairPhones) {
    ui.press(Key::BootTap, 0);
  }
  ui.press(Key::Power, 0);
  CHECK(ui.snapshot().peer_count == 2);

  bonds.answers = false;                // the radio rested under the screen
  ui.press(Key::BootTap, 0);            // a press still reads the table
  CHECK(ui.snapshot().peer_count == 2); // the answer it could not get changes nothing
  const ScreenDraw list = ui.app().frame();
  CHECK(list.unpair.count == 2);
  CHECK(std::string(list.unpair.row[1].addr) == "AA:BB:CC:DD:EE:02");

  // And the store speaking again is believed, empty or not.
  bonds.answers = true;
  bonds.peers.clear();
  ui.press(Key::BootTap, 0);
  CHECK(ui.snapshot().peer_count == 0);
  CHECK(ui.app().frame().unpair.empty);
}

TEST("unpair: a BOOT hold removes the selected phone once, and the list shrinks") {
  TempDir dir;
  Store store(dir.path + "/notes");
  CHECK(store.ensure());
  FakeBonds bonds;
  bonds.peers.push_back(make_peer("AA:BB:CC:DD:EE:01"));
  bonds.peers.push_back(make_peer("AA:BB:CC:DD:EE:02"));

  // The adapter owns the card, the machine and the bond store together: this is
  // the same path the device runs, with the radio replaced by FakeBonds.
  UiState ui;
  Snapshot seed;
  seed.card = true;
  ui.begin(seed, &store, &bonds);

  ui.press(Key::BootHold, 0);  // Home -> menu
  CHECK(ui.screen() == Screen::Menu);
  while (static_cast<MenuItem>(ui.app().state().menu) != MenuItem::UnpairPhones) {
    ui.press(Key::BootTap, 0);
  }
  ui.press(Key::Power, 0);  // select the row
  CHECK(ui.screen() == Screen::UnpairList);
  CHECK(ui.snapshot().peer_count == 2);  // the table was read on the way in
  CHECK(std::string(ui.app().frame().unpair.row[0].addr) == "AA:BB:CC:DD:EE:01");
  CHECK(bonds.erases == 0);

  // The second phone, asked about: the question states the address it removes.
  ui.press(Key::BootTap, 0);
  CHECK(ui.app().frame().unpair.selected == 1);
  CHECK(ui.press(Key::Power, 0).effect == Effect::None);
  CHECK(ui.screen() == Screen::ConfirmUnpair);
  const ConfirmView ask = ui.app().frame().confirm;
  CHECK(std::string(ask.hero) == "UNPAIR?");
  CHECK(std::string(ask.line1) == "AA:BB:CC:DD:EE:02");
  CHECK(std::string(ask.hold) == "HOLD: unpair");
  CHECK(ask.why == nullptr);
  CHECK(bonds.erases == 0);  // the question deletes nothing

  // The hold: one erase, and the list the glass comes back to is one row shorter.
  const Outcome hold = ui.press(Key::BootHold, 0);
  CHECK(hold.effect == Effect::UnpairPeer);
  CHECK(hold.peer == 1);
  CHECK(ui.screen() == Screen::UnpairList);
  const Outcome after = ui.unpair(hold.peer, 0);
  CHECK(bonds.erases == 1);
  CHECK(bonds.peers.size() == 1);
  CHECK(std::string(bonds.peers[0].name) == "");  // the one left is the other phone
  CHECK(after.draw);
  CHECK(after.refresh == Refresh::Full);
  CHECK(ui.snapshot().peer_count == 1);
  const ScreenDraw list = ui.app().frame();
  CHECK(list.unpair.count == 1);
  CHECK(std::string(list.unpair.row[0].addr) == "AA:BB:CC:DD:EE:01");

  // Doing it again with nothing selected but the last phone leaves an empty list:
  // the hold is one erase per press, never a loop that empties the table.
  ui.press(Key::Power, 0);
  CHECK(ui.screen() == Screen::ConfirmUnpair);
  const Outcome last = ui.press(Key::BootHold, 0);
  CHECK(last.effect == Effect::UnpairPeer);
  CHECK(last.peer == 0);
  ui.unpair(last.peer, 0);
  CHECK(bonds.erases == 2);
  CHECK(ui.snapshot().peer_count == 0);
  CHECK(ui.app().frame().unpair.empty);
}

TEST("unpair: a tap keeps the phone, and a store that refuses keeps the row") {
  TempDir dir;
  Store store(dir.path + "/notes");
  CHECK(store.ensure());
  FakeBonds bonds;
  bonds.peers.push_back(make_peer("AA:BB:CC:DD:EE:01"));

  UiState ui;
  Snapshot seed;
  seed.card = true;
  ui.begin(seed, &store, &bonds);
  ui.press(Key::BootHold, 0);
  while (static_cast<MenuItem>(ui.app().state().menu) != MenuItem::UnpairPhones) {
    ui.press(Key::BootTap, 0);
  }
  ui.press(Key::Power, 0);
  CHECK(ui.screen() == Screen::UnpairList);

  // BOOT tap on the question: the safe answer, the list back, nothing deleted.
  CHECK(ui.press(Key::Power, 0).effect == Effect::None);
  CHECK(ui.screen() == Screen::ConfirmUnpair);
  const Outcome tap = ui.press(Key::BootTap, 0);
  CHECK(tap.effect == Effect::None);
  CHECK(ui.screen() == Screen::UnpairList);
  CHECK(bonds.erases == 0);
  CHECK(bonds.peers.size() == 1);

  // PWR tap is the same safe answer (the delete confirm's rule).
  CHECK(ui.press(Key::Power, 0).effect == Effect::None);
  CHECK(ui.screen() == Screen::ConfirmUnpair);
  CHECK(ui.press(Key::Power, 0).effect == Effect::None);
  CHECK(ui.screen() == Screen::UnpairList);
  CHECK(bonds.erases == 0);

  // The store says no (a phone unpaired from the console a moment earlier): the
  // erase is asked once, and no row leaves the glass.
  bonds.refused = 1;
  CHECK(ui.press(Key::Power, 0).effect == Effect::None);  // the question again
  CHECK(ui.screen() == Screen::ConfirmUnpair);
  const Outcome hold = ui.press(Key::BootHold, 0);
  CHECK(hold.effect == Effect::UnpairPeer);
  const Outcome refused = ui.unpair(hold.peer, 0);
  CHECK(bonds.erases == 1);
  CHECK(bonds.peers.size() == 1);
  CHECK(!refused.draw);
  CHECK(ui.snapshot().peer_count == 1);

  // And the hold on the list itself leaves for the menu with the bond intact.
  // The hold that asked about the last phone was the walk's fourth hand-over, so
  // it spent the ghost budget's flash; leaving the list is a plain partial again.
  CHECK(ui.press(Key::BootHold, 0).refresh == Refresh::Partial);
  CHECK(ui.screen() == Screen::Menu);
  CHECK(bonds.erases == 1);
  CHECK(bonds.peers.size() == 1);
}

TEST("guard: an unpair is refused while the device is busy") {
  Snapshot snap;
  snap.peers[0] = make_peer("AA:BB:CC:DD:EE:01");
  snap.peer_count = 1;
  std::snprintf(snap.busy, sizeof snap.busy, "BUSY UPLOADING");

  ScreenApp app;
  app.begin(snap);
  open_menu_row(app, MenuItem::UnpairPhones);
  CHECK(app.screen() == Screen::UnpairList);
  CHECK(app.press(Key::Power, 0).effect == Effect::None);
  CHECK(app.screen() == Screen::ConfirmUnpair);

  // The reason is on the question, and the hold does nothing at all.
  const ScreenDraw frame = app.frame();
  CHECK(frame.confirm.why != nullptr);
  CHECK(std::string(frame.confirm.why) == "BUSY UPLOADING");
  const Outcome held = app.press(Key::BootHold, 0);
  CHECK(held.effect == Effect::None);
  CHECK(held.effect != Effect::UnpairPeer);
  CHECK(app.screen() == Screen::ConfirmUnpair);
  CHECK(!held.draw);  // the line is already on the glass

  // Idle again: the same hold goes through.
  Snapshot idle = snap;
  idle.busy[0] = '\0';
  ScreenApp free;
  free.begin(idle);
  open_menu_row(free, MenuItem::UnpairPhones);
  free.press(Key::Power, 0);
  CHECK(free.frame().confirm.why == nullptr);
  CHECK(free.press(Key::BootHold, 0).effect == Effect::UnpairPeer);
}

TEST("home: shows 0 NOTES after a wipe") {
  TempDir dir;
  Store store(dir.path + "/notes");
  CHECK(!make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 14).empty());
  CHECK(!make_note(store, "hg-a-20261120T091100-0002", "2026-11-20T09:11:00Z", 60, NoteState::Sent).empty());

  Cache cache;
  CHECK(cache.reload(store));
  Snapshot snap = snapshot_for(cache, store, Screen::Home, 0);
  snap.card = true;
  snap.count_pending = cache.pending();
  snap.count_sent = cache.sent();
  snap.count_failed = cache.failed();
  snap.note_count = static_cast<uint16_t>(cache.note_count());

  ScreenApp app;
  app.begin(snap);
  CHECK(std::string(build_menu(snap, 0).rows[0].value) == "2");

  open_menu_row(app, MenuItem::WipeNotes);
  const Outcome wipe = app.press(Key::BootHold, 0);
  CHECK(wipe.effect == Effect::WipeNotes);
  CHECK(app.screen() == Screen::Wiping);

  // What the device's effect does (ui_device -> Runtime::begin_wipe, then the
  // worker's own walk on the card), and the two moves the app task makes on the
  // glass while it runs: the counts, then Home.
  CHECK(store.wipe() == 2);
  CHECK(cache.reload(store));
  Snapshot after = snapshot_for(cache, store, Screen::Home, 0);
  after.card = true;
  after.count_pending = cache.pending();
  after.count_sent = cache.sent();
  after.count_failed = cache.failed();
  after.note_count = static_cast<uint16_t>(cache.note_count());
  CHECK(after.notes_total() == 0);

  const Outcome publish = app.publish(after, static_cast<uint16_t>(DirtyQueue | DirtyList));
  CHECK(publish.draw);
  // The job is over: the machine's own move back to Home, then the publish that
  // carries the empty card (t_0070a731).
  const Outcome home = app.card_job_finished();
  CHECK(home.draw);
  CHECK(home.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Home);
  const ScreenDraw frame = app.frame();
  CHECK(std::string(frame.home.band) == "ALL SENT");
  CHECK(std::string(frame.home.sub) == "BOOT TO RECORD");  // no LAST, nothing left
  CHECK(std::string(build_menu(after, 0).rows[0].value) == "0");

  // The store itself: every state emptied, the folders kept, so the device is
  // usable straight after a wipe.
  CHECK(store.count(NoteState::Pending) == 0);
  CHECK(store.count(NoteState::Sent) == 0);
  CHECK(store.count(NoteState::Failed) == 0);
  for (NoteState state : {NoteState::Pending, NoteState::Sent, NoteState::Failed}) {
    CHECK(access(store.state_dir(state).c_str(), F_OK) == 0);
  }
}

TEST("store: wipe removes every note and leaves the folders") {
  TempDir dir;
  Store store(dir.path + "/notes");
  store.ensure();
  CHECK(!make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 14, NoteState::Pending).empty());
  CHECK(!make_note(store, "hg-a-20261120T091100-0002", "2026-11-20T09:11:00Z", 3, NoteState::Failed).empty());
  CHECK(!make_note(store, "hg-a-20261119T184000-0003", "2026-11-19T18:40:00Z", 60, NoteState::Sent).empty());
  CHECK(store.count(NoteState::Pending) == 1);

  CHECK(store.wipe() == 3);
  CHECK(store.count(NoteState::Pending) == 0);
  CHECK(store.count(NoteState::Sent) == 0);
  CHECK(store.count(NoteState::Failed) == 0);
  CHECK(store.list(NoteState::Pending).empty());
  // Nothing to wipe twice, and nothing lost that the store needs: the three
  // state folders are there, so the next note has somewhere to land.
  CHECK(store.wipe() == 0);
  for (NoteState state : {NoteState::Pending, NoteState::Sent, NoteState::Failed}) {
    CHECK(access(store.state_dir(state).c_str(), F_OK) == 0);
  }
  CHECK(store.ensure());
}

TEST("busy: a wipe draws its face before the work and Home after it") {
  // t_0070a731: the confirm's answer used to be the work, so the glass stayed on
  // the confirm for the whole 90 s it took. Now the hold returns the busy face
  // with the effect, the device paints that frame before it starts the job, and
  // the Home frame comes only when the job is over.
  Snapshot snap;
  snap.card = true;
  snap.note_count = 3;
  snap.count_pending = 3;

  ScreenApp app;
  app.begin(snap);
  open_menu_row(app, MenuItem::WipeNotes);
  CHECK(app.screen() == Screen::ConfirmWipe);

  const Outcome wipe = app.press(Key::BootHold, 0);
  CHECK(wipe.effect == Effect::WipeNotes);  // what ui_device runs after the frame
  CHECK(wipe.draw);
  CHECK(wipe.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Wiping);

  // The frame the device draws before it calls the runtime: WIPING, and the card
  // is still being read (the total arrives with the worker's first progress).
  const ScreenDraw before = app.frame();
  CHECK(before.screen == Screen::Wiping);
  CHECK(std::string(before.busy.hero) == "WIPING");
  CHECK(std::string(before.busy.line1) == "READING CARD");
  CHECK(std::string(before.busy.line2) == "THE CARD STAYS");
  CHECK(!before.busy.bar);

  // The counts, as the store's own walk reports them: 114 notes on the bench.
  const Outcome known = app.card_job_progress(false, 0, 114);
  CHECK(known.draw);
  CHECK(known.refresh == Refresh::Partial);
  const ScreenDraw first = app.frame();
  CHECK(std::string(first.busy.line1) == "0 OF 114");
  CHECK(first.busy.bar);
  CHECK(first.busy.pct == 0);

  // A whole-panel waveform is ~0.6 s on this unit, so the bar moves on a 5 %
  // step and not on a note: 1 of 114 is still 0 %, 6 of 114 is the next frame.
  CHECK(!app.card_job_progress(false, 1, 114).draw);
  const Outcome step = app.card_job_progress(false, 6, 114);
  CHECK(step.draw);
  CHECK(step.refresh == Refresh::Partial);
  CHECK(std::string(app.frame().busy.line1) == "6 OF 114");
  CHECK(app.frame().busy.pct == 5);
  // ...and the last note always draws, however small its step.
  CHECK(app.card_job_progress(false, 114, 114).draw);
  CHECK(app.frame().busy.pct == 100);

  // No key reaches the job: a tap, a hold and a PWR hold all do nothing.
  CHECK(app.press(Key::BootTap, 0).effect == Effect::None);
  CHECK(app.press(Key::BootHold, 0).effect == Effect::None);
  CHECK(app.press(Key::Power, 0).effect == Effect::None);
  CHECK(app.press(Key::PowerHold, 0).effect == Effect::None);
  CHECK(app.screen() == Screen::Wiping);

  // The end: Home, full, and the publish that follows reads 0 NOTES.
  const Outcome done = app.card_job_finished();
  CHECK(done.draw);
  CHECK(done.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Home);
  Snapshot empty = snap;
  empty.note_count = 0;
  empty.count_pending = 0;
  CHECK(app.publish(empty, static_cast<uint16_t>(DirtyQueue | DirtyList)).draw);
  CHECK(std::string(app.frame().home.band) == "ALL SENT");
}

TEST("busy: a format says what it is doing and draws no invented progress") {
  // A format has no number the firmware can honestly report (one 22.8 s
  // esp_vfs_fat_sdcard_format call), so its face carries the word, the target
  // and the one instruction that matters, and no bar.
  Snapshot snap;
  snap.card = true;
  ScreenApp app;
  app.begin(snap);
  open_menu_row(app, MenuItem::FormatCard);
  CHECK(app.press(Key::BootHold, 0).effect == Effect::FormatCard);
  CHECK(app.screen() == Screen::Wiping);

  const ScreenDraw face = app.frame();
  CHECK(face.screen == Screen::Wiping);
  CHECK(std::string(face.busy.hero) == "FORMATTING");
  CHECK(std::string(face.busy.line1) == "THE SD CARD");
  CHECK(std::string(face.busy.line2) == "DO NOT UNPLUG");
  CHECK(!face.busy.bar);
  // The runtime reports no counts for it: no frame is owed for 0 of 0.
  CHECK(!app.card_job_progress(true, 0, 0).draw);

  CHECK(app.card_job_finished().draw);
  CHECK(app.screen() == Screen::Home);
}

TEST("store: wipe reports (done, total) for every note, and a false stops it") {
  // The count the busy face draws is the store's own: the total is known before
  // the first removal (every folder is listed first), and one pair arrives per
  // note. A false answer - a shutdown - stops the walk where it is.
  TempDir dir;
  Store store(dir.path + "/notes");
  store.ensure();
  CHECK(!make_note(store, "hg-a-20261121T140200-0001", "2026-11-21T14:02:00Z", 1, NoteState::Pending).empty());
  CHECK(!make_note(store, "hg-a-20261121T140201-0002", "2026-11-21T14:02:01Z", 1, NoteState::Pending).empty());
  CHECK(!make_note(store, "hg-a-20261121T140202-0003", "2026-11-21T14:02:02Z", 1, NoteState::Pending).empty());
  CHECK(!make_note(store, "hg-a-20261120T091100-0004", "2026-11-20T09:11:00Z", 1, NoteState::Failed).empty());
  CHECK(!make_note(store, "hg-a-20261119T184000-0005", "2026-11-19T18:40:00Z", 1, NoteState::Sent).empty());

  std::vector<std::pair<uint32_t, uint32_t>> calls;
  const uint32_t gone = store.wipe([&calls](uint32_t done, uint32_t total) {
    calls.push_back({done, total});
    return true;
  });
  CHECK(gone == 5);
  CHECK(calls.size() == 6);  // 0 of 5 first, then one pair a note
  CHECK(calls.front().first == 0);
  CHECK(calls.front().second == 5);
  CHECK(calls.back().first == 5);
  CHECK(calls.back().second == 5);
  for (size_t i = 1; i < calls.size(); ++i) {
    CHECK(calls[i].first == calls[i - 1].first + 1);
    CHECK(calls[i].second == 5);  // the total never moves
  }

  // A second walk stops where the callback says so: two removed, three left.
  CHECK(!make_note(store, "hg-a-20261118T100000-0006", "2026-11-18T10:00:00Z", 1).empty());
  CHECK(!make_note(store, "hg-a-20261118T100001-0007", "2026-11-18T10:00:01Z", 1).empty());
  CHECK(!make_note(store, "hg-a-20261118T100002-0008", "2026-11-18T10:00:02Z", 1).empty());
  uint32_t seen = 0;
  const uint32_t stopped = store.wipe([&seen](uint32_t done, uint32_t total) {
    (void)total;
    seen = done;
    return done < 2;  // stop once two notes are gone
  });
  CHECK(stopped == 2);
  CHECK(seen == 2);
  CHECK(store.count(NoteState::Pending) == 1);
}
