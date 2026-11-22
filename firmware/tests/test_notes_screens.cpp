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
#include <string>
#include <vector>

#include "hg/canvas.hpp"
#include "notes/notes_capture.hpp"
#include "notes/notes_queue.hpp"
#include "notes/notes_store.hpp"
#include "notes_app/strings.hpp"
#include "notes_app/ui_app.hpp"
#include "notes_app/ui_cache.hpp"
#include "notes_app/ui_render.hpp"
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

}  // namespace

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

  // Home -> Menu -> Notes -> Note -> Confirm -> back out.
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Menu);
  // BOOT walks the rows; PWR selects. Walk to About, then run it.
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::About) {
    CHECK(app.press(Key::BootTap, 0).refresh == Refresh::Partial);
  }
  const Outcome about = app.press(Key::Power, 0);
  CHECK(about.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::About);
  CHECK(app.press(Key::Power, 0).effect == Effect::PowerOff);  // PWR is power, not select
  CHECK(app.screen() == Screen::About);  // the effect happens; the page stays put
  CHECK(app.press(Key::BootHold, 0).refresh == Refresh::Full);  // hold: back to Menu
  CHECK(app.screen() == Screen::Menu);

  // Back to Notes and in.
  while (static_cast<MenuItem>(app.state().menu) != MenuItem::Notes) app.press(Key::BootTap, 0);
  const Outcome open_list = app.press(Key::Power, 0);
  CHECK(open_list.effect == Effect::None);
  CHECK(app.screen() == Screen::NoteList);
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

TEST("about: a hold goes back to the menu, a tap does nothing, PWR is power") {
  State s;
  s.screen = Screen::About;
  CHECK(press_boot(s) == Action::None);
  CHECK(press_power(s) == Action::PowerOff);  // PWR is power here, as outside the menu
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
  std::snprintf(frame.confirm.when, sizeof frame.confirm.when, "21 Nov 14:02");
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

  static uint16_t fb[200 * 200];
  const Screen screens[] = {Screen::Home,   Screen::Menu,     Screen::NoteList,
                            Screen::NoteAction, Screen::ConfirmDelete, Screen::Playing,
                            Screen::Recording, Screen::Pairing, Screen::Phone, Screen::About};
  for (Screen screen : screens) {
    frame.screen = screen;
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

TEST("render: the confirm has no footer, the list marks the exception") {
  static uint16_t fb[200 * 200];
  std::memset(fb, 0xFF, sizeof fb);
  hg::Canvas canvas(fb, 200, 200, false);
  ScreenDraw frame;
  frame.screen = Screen::ConfirmDelete;
  std::snprintf(frame.confirm.when, sizeof frame.confirm.when, "21 Nov 14:02");
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
