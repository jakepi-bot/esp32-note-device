// The screens' refresh policy, driven without a board (QA D1).
//
// test_notes_screens.cpp feeds hand-built Snapshots straight into ScreenApp: it
// checks what the glass says, never when the glass is told to say it. That is
// exactly the layer the QA pass found broken - the snapshot was rebuilt only
// from callbacks nothing called, so a note recorded after power-on never reached
// the glass and Home's band and hero froze at the boot frame. These cases drive
// notesui::UiState - the adapter's policy - against a real card in a temp
// directory, so the event-on-the-card -> new-frame rule is a test.
#include "check.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "notes/notes_capture.hpp"
#include "notes/notes_store.hpp"
#include "notes_app/strings.hpp"
#include "notes_app/ui_state.hpp"
#include "notes_app/ui_view.hpp"

namespace notes = hgp::notes;
using namespace hgp::notesui;
using hgp::notes::Meta;
using hgp::notes::NoteState;
using hgp::notes::Store;

namespace {

// The stamps are local time; a fixed TZ keeps the strings identical on every
// host (the device gets its TZ from port_time).
struct FixedZone {
  FixedZone() {
    setenv("TZ", "UTC", 1);
    tzset();
  }
} g_fixed_zone;

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/hg-ui-refresh-XXXXXX";
    const char* made = mkdtemp(tmpl);
    path = made ? made : "/tmp/hg-ui-refresh-fallback";
  }
  ~TempDir() {
    const std::string cmd = "rm -rf '" + path + "'";
    if (std::system(cmd.c_str()) != 0) return;
  }
};

// A note on the card, the way the runtime commits one: the folder and the
// note.json that makes it exist (no audio: the list and the counts do not read
// it).
bool make_note(Store& store, const std::string& id, const char* recorded_at, uint32_t seconds = 2,
               NoteState where = NoteState::Pending) {
  std::string error;
  if (!store.ensure()) return false;
  if (!store.create_note(id, error)) return false;
  Meta meta;
  meta.note_id = id;
  meta.device_id = "hg-test";
  meta.device_name = "test";
  meta.firmware = "0.0.0";
  meta.recorded_at = recorded_at;
  meta.duration_s = seconds;
  meta.bytes = seconds * notes::kSampleRate * 2;
  meta.idempotency_key = id;
  if (!store.write_meta(id, meta, error)) return false;
  return where == NoteState::Pending || store.move(id, NoteState::Pending, where);
}

// A seed the way ui_device.cpp builds one: the strings and the cap, no counts.
Snapshot seed_for(const char* firmware = "0.7.1", bool wifi = false, bool clock = true) {
  Snapshot seed;
  std::snprintf(seed.firmware, sizeof seed.firmware, "%s", firmware);
  std::snprintf(seed.device, sizeof seed.device, "%s", "notes-test");
  seed.note_max_s = 300;
  seed.wifi_up = wifi;
  seed.clock_set = clock;
  return seed;
}

}  // namespace

TEST("refresh: a note committed after boot reaches the next frame (QA D1)") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(seed_for(), &store);
  CHECK(ui.snapshot().note_count == 0);
  CHECK(ui.snapshot().card);
  CHECK(std::string(build_home(ui.snapshot()).sub) == sub::kBootToRecord);

  // The runtime commits a note behind the adapter's back, then says so - the
  // publish the QA pass found missing entirely.
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0001", "2026-11-21T14:02:03Z"));
  const Outcome out = ui.publish(1000, static_cast<uint16_t>(DirtyQueue | DirtyList));
  CHECK(out.draw);
  CHECK(ui.snapshot().note_count == 1);
  CHECK(ui.snapshot().count_pending == 1);

  const HomeView view = build_home(ui.snapshot());
  CHECK(std::string(view.band) == "1 TO SEND");
  CHECK(std::string(view.hero) == "OFFLINE");  // one to send, no association
  CHECK(std::string(view.sub) == "LAST 14:02");
}

TEST("refresh: the boot frame knows the card it booted with (QA D2)") {
  TempDir dir;
  Store store(dir.path);
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0002", "2026-11-21T14:02:03Z"));

  UiState ui;
  ui.begin(seed_for(), &store);
  // begin() fills the window after the machine has the counts, so the very first
  // frame carries the newest note - not "BOOT TO RECORD" over 58 sent notes.
  const HomeView view = build_home(ui.snapshot());
  CHECK(ui.snapshot().note_count == 1);
  CHECK(std::string(view.sub) == "LAST 14:02");
  CHECK(std::string(view.band) == "1 TO SEND");
}

TEST("refresh: a key press reads the card before the screen machine acts (QA D1)") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(seed_for(), &store);
  CHECK(ui.app().state().note_count == 0);

  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0003", "2026-11-21T14:02:03Z"));
  // No publish: the press itself has to see the note, or the list opens empty.
  ui.press(Key::BootHold, 2000);  // Home -> menu
  CHECK(ui.screen() == Screen::Menu);
  ui.press(Key::Power, 3000);  // the Notes row: PWR selects
  CHECK(ui.screen() == Screen::NoteList);
  CHECK(ui.app().state().note_count == 1);
  CHECK(ui.snapshot().note_count == 1);

  const Window window = ui.window();
  CHECK(window.first == 0);
  CHECK(window.rows == 1);
  const ListView list = build_list(ui.snapshot(), 0);
  CHECK(list.count == 1);
  CHECK(list.rows == 1);
  CHECK(std::string(list.row[0].when) == "21 Nov 14:02");
}

TEST("refresh: the list the press draws has its rows, not just its title (t_8c3e4900)") {
  TempDir dir;
  Store store(dir.path);
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0003", "2026-11-21T14:02:03Z", 2, NoteState::Pending));
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140501-0003", "2026-11-21T14:05:01Z", 3, NoteState::Failed));
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140804-0003", "2026-11-21T14:08:04Z", 4, NoteState::Sent));
  UiState ui;
  ui.begin(seed_for(), &store);

  // No publish between the notes landing and the press: the frame this press
  // draws is the only chance the rows have to reach the glass.
  const Outcome into_menu = ui.press(Key::BootHold, 2000);  // Home -> menu
  CHECK(into_menu.draw);
  const Outcome into_list = ui.press(Key::Power, 3000);  // the Notes row: PWR selects
  CHECK(into_list.draw);
  CHECK(ui.screen() == Screen::NoteList);

  // What the panel is handed, not what the adapter cached: the machine was
  // published the snapshot before it acted, and that one's window was zeroed by
  // load_counts(), so the list drew its title line over an empty body.
  const ScreenDraw drawn = ui.app().frame();
  CHECK(drawn.screen == Screen::NoteList);
  CHECK(drawn.list.count == 3);
  CHECK(drawn.list.rows == 3);
  CHECK(std::string(drawn.list.row[0].when) == "21 Nov 14:08");  // newest first
  CHECK(drawn.list.row[0].mark == ' ');
  CHECK(std::string(drawn.list.row[1].when) == "21 Nov 14:05");
  CHECK(drawn.list.row[1].mark == '!');
  CHECK(std::string(drawn.list.row[2].when) == "21 Nov 14:02");
  CHECK(drawn.list.row[2].mark == '*');
  CHECK(std::string(drawn.list.key_line) == list::kFailed);
}

TEST("refresh: an upload's progress and its failure reach the glass") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(seed_for(), &store);
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0004", "2026-11-21T14:02:03Z"));
  ui.publish(1000, static_cast<uint16_t>(DirtyQueue | DirtyList));

  ui.set_upload(true, 1, 3);
  const Outcome running = ui.publish(2000, DirtyUpload);
  CHECK(running.draw);
  HomeView view = build_home(ui.snapshot());
  CHECK(ui.snapshot().upload_active);
  CHECK(std::string(view.hero) == home::kUploading);
  CHECK(std::string(view.sub) == "1 OF 3 SENT");
  CHECK(view.bar_pct == 33);

  ui.set_upload_failed();
  const Outcome failed = ui.publish(3000, DirtyUpload);
  CHECK(failed.draw);
  view = build_home(ui.snapshot());
  CHECK(ui.snapshot().upload_failed);
  CHECK(!ui.snapshot().upload_active);
  CHECK(std::string(view.hero) == home::kNoSync);
}

TEST("refresh: the link, the clock and the card's space are facts, not once-at-boot") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(seed_for("0.7.1", false, true), &store);

  Facts facts;
  facts.card_free_bytes = 62900000000ull;  // 62.9 GB, the desk card's answer
  const Outcome out = ui.set_facts(facts, 1000);
  CHECK(out.draw);
  CHECK(ui.snapshot().card_free_bytes == 62900000000ull);
  CHECK(std::string(build_about(ui.snapshot()).value[3]) == "62.9G");  // QA D3

  Facts offline = facts;
  offline.wifi_up = true;
  CHECK(ui.set_facts(offline, 2000).draw);  // the link came up: the band moves
  CHECK(ui.snapshot().wifi_up);

  // The same facts again cost no frame.
  CHECK(!ui.set_facts(offline, 3000).draw);
}

TEST("refresh: the list says when it is not the whole card (QA D7)") {
  TempDir dir;
  Store store(dir.path);
  // One more note than the list keeps: the count line has to admit the window.
  for (unsigned i = 0; i <= kMaxListRows; ++i) {
    char id[64];
    std::snprintf(id, sizeof id, "hg-69ffb4e9-20261121T140203-%04u", i);
    CHECK(make_note(store, id, "2026-11-21T14:02:03Z", 2, NoteState::Sent));
  }
  UiState ui;
  ui.begin(seed_for(), &store);
  CHECK(ui.snapshot().count_sent == kMaxListRows + 1);  // the true total
  CHECK(ui.snapshot().count_pending == 0);
  CHECK(ui.snapshot().note_count == kMaxListRows);          // what the list holds
  CHECK(ui.snapshot().list_truncated);
  CHECK(ui.snapshot().notes_total() == kMaxListRows + 1);

  const ListView list = build_list(ui.snapshot(), 0);
  CHECK(list.key_line == nullptr);
  CHECK(std::string(list.key_count) == "128+");
}

TEST("capture: a queued tap survives a publish and counts from the capture, not the tap") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(seed_for(), &store);

  // Home, BOOT tap: the glass moves to the recording face at once.
  ui.press(Key::BootTap, 1000);
  CHECK(ui.screen() == Screen::Recording);

  // The runtime kept the tap because an upload was in flight: the glass draws the
  // wait where the counter would be.
  const Outcome queued = ui.set_capture(false, 0, true, 2000);
  CHECK(queued.draw);
  CHECK(ui.snapshot().record_queued);
  CHECK(ui.app().frame().recording.queued);

  // The upload steps and republishes the card while the tap waits. The wait has
  // to survive the publish: the snapshot the adapter hands the screen machine is
  // its own, and it carried no arm before this test existed.
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0005", "2026-11-21T14:02:03Z"));
  ui.publish(3000, static_cast<uint16_t>(DirtyQueue | DirtyList));
  CHECK(ui.snapshot().record_queued);
  CHECK(ui.app().frame().recording.queued);

  // Three minutes later the upload ends and the runtime starts the capture. The
  // counter is the capture's own second, not the 180 s the press waited.
  const Outcome started = ui.set_capture(true, 0, false, 183000);
  CHECK(started.draw);
  CHECK(!ui.app().frame().recording.queued);
  CHECK(ui.app().frame().recording.elapsed_s == 0);
  CHECK(!ui.tick(183500).draw);
  CHECK(ui.app().frame().recording.elapsed_s == 0);
  CHECK(ui.tick(214000).draw);  // the 30 s partial
  CHECK(ui.app().frame().recording.elapsed_s == 31);
}

// ---------------------------------------------------------------------------
// The flicker budget (t_3beb371a §5): the clock repaints once a minute, the
// battery waits for a real move, and a capture silences the whole path.

namespace {

Snapshot battery_seed(int8_t pct) {
  Snapshot seed;
  std::snprintf(seed.firmware, sizeof seed.firmware, "%s", "0.7.1");
  std::snprintf(seed.device, sizeof seed.device, "%s", "notes-test");
  seed.note_max_s = 300;
  seed.wifi_up = true;
  seed.clock_set = true;
  seed.battery_pct = pct;
  return seed;
}

Facts facts_at(int8_t pct, bool clock = true) {
  Facts f;
  f.wifi_up = true;
  f.clock_set = clock;
  f.battery_pct = pct;
  return f;
}

}  // namespace

TEST("facts: a minute rollover is not a frame; the clock's set answer is") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(battery_seed(84), &store);

  // The seed says the clock is set, and the minute is no longer drawn: however
  // many polls pass, and whatever the wall clock reads, nothing moves.
  for (uint32_t t = 1000; t <= 61000; t += 10000) CHECK(!ui.set_facts(facts_at(84), t).draw);
  CHECK(build_home(ui.snapshot()).clock_msg[0] == '\0');

  // The one clock fact the band still draws is its set/unset answer: the
  // warning appears when the clock goes away, and clears when it is back. It is
  // a band-slot change, so partial - never the hero's full flash.
  const Outcome shown = ui.set_facts(facts_at(84, false), 71000);
  CHECK(shown.draw);
  CHECK(shown.refresh == Refresh::Partial);
  CHECK(std::string(build_home(ui.snapshot()).clock_msg) == "NO TIME");
  CHECK(!ui.set_facts(facts_at(84, false), 81000).draw);
  const Outcome cleared = ui.set_facts(facts_at(84, true), 91000);
  CHECK(cleared.draw);
  CHECK(cleared.refresh == Refresh::Partial);
  CHECK(build_home(ui.snapshot()).clock_msg[0] == '\0');

  // Away from Home the answer is adopted but not drawn; the full frame that
  // comes back Home carries it (a press to the menu is Full).
  CHECK(ui.press(Key::BootHold, 92000).draw);
  CHECK(ui.screen() == Screen::Menu);
  const Outcome away = ui.set_facts(facts_at(84, false), 101000);
  CHECK(!away.draw);
  CHECK(!ui.snapshot().clock_set);
}

TEST("facts: a one percent battery step is the ADC talking, not the glass") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(battery_seed(84), &store);
  // Nothing has moved yet: the clock no longer opens with a minute of its own.
  CHECK(!ui.set_facts(facts_at(84), 5000).draw);
  CHECK(!ui.set_facts(facts_at(84), 9000).draw);

  // Nothing but the battery moved, and it moved by the ADC's own noise.
  CHECK(!ui.set_facts(facts_at(83), 10000).draw);
  CHECK(ui.snapshot().battery_pct == 84);  // the glass keeps what it drew

  // Two percent is a real step, and it draws.
  CHECK(ui.set_facts(facts_at(82), 20000).draw);
  CHECK(ui.snapshot().battery_pct == 82);
  CHECK(!ui.set_facts(facts_at(82), 30000).draw);

  // One more percent waits, then draws once five minutes have passed since the
  // last power publish - the level cannot drift far from the glass silently.
  CHECK(!ui.set_facts(facts_at(81), 120000).draw);
  CHECK(ui.snapshot().battery_pct == 82);
  CHECK(ui.set_facts(facts_at(81), 320000).draw);
  CHECK(ui.snapshot().battery_pct == 81);

  // Plugging in is not a percentage: it always draws.
  Facts cable = facts_at(81);
  cable.external = true;
  CHECK(ui.set_facts(cable, 330000).draw);
  CHECK(ui.snapshot().external);
}

TEST("battery: a low cell says so once, and a charge re-arms the warning") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(battery_seed(12), &store);
  // Above the line: nothing to say.
  CHECK(!ui.set_facts(facts_at(12), 5000).draw);
  CHECK(std::string(ui.snapshot().notice).empty());

  // Crossing it draws, and Home says why - once.
  CHECK(ui.set_facts(facts_at(9), 20000).draw);
  CHECK(std::string(ui.snapshot().notice) == "BATTERY LOW");

  // The line lapses on its own - tick() is the clock that watches for it - and
  // is not said again while the cell stays low.
  ui.tick(60000);
  CHECK(std::string(ui.snapshot().notice).empty());
  ui.set_facts(facts_at(8), 400000);
  CHECK(std::string(ui.snapshot().notice).empty());

  // A charge re-arms it, so the next fall earns its own warning.
  Facts cable = facts_at(8);
  cable.external = true;
  ui.set_facts(cable, 500000);
  ui.set_facts(facts_at(40), 600000);
  CHECK(ui.set_facts(facts_at(9), 700000).draw);
  CHECK(std::string(ui.snapshot().notice) == "BATTERY LOW");
}

TEST("notice: a discarded capture's reason takes Home's line, then the counts return") {
  TempDir dir;
  Store store(dir.path);
  // A card with one sent note: Home's resting line is LAST, the line the notice
  // has to give back.
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0006", "2026-11-21T14:02:03Z", 40,
                  NoteState::Sent));
  UiState ui;
  ui.begin(seed_for(), &store);
  HomeView view = build_home(ui.snapshot());
  CHECK(std::string(view.hero) == home::kReady);
  CHECK(std::string(view.sub) == "LAST 14:02");

  // The stop found no audio and the runtime signalled it (NotesUi::on_discarded
  // hands Home the line, then publishes): the frame is Home once, partial, with
  // the reason where the counts line was.
  ui.set_notice(sub::kNoAudioSaved, 2000, 5000);
  const Outcome drawn = ui.publish(2000, DirtyNotice, Refresh::Partial);
  CHECK(drawn.draw);
  CHECK(drawn.refresh == Refresh::Partial);  // one line on one screen, never full
  view = ui.app().frame().home;              // the frame the panel is given
  CHECK(std::string(view.sub) == "NO AUDIO SAVED");
  CHECK(std::string(view.hero) == home::kReady);  // the hero is not the notice's
  CHECK(std::string(view.band) == band::kAllSent);
  CHECK(ui.snapshot().notes_total() == 1);   // nothing was saved

  // Inside its few seconds nothing repaints.
  CHECK(!ui.tick(4000).draw);
  CHECK(std::string(ui.app().frame().home.sub) == "NO AUDIO SAVED");

  // Its ttl runs out: the next pass puts the counts line back, once.
  const Outcome lapsed = ui.tick(7000);
  CHECK(lapsed.draw);
  view = ui.app().frame().home;
  CHECK(std::string(view.sub) == "LAST 14:02");
  CHECK(ui.snapshot().notice[0] == '\0');
  CHECK(!ui.tick(7100).draw);  // and the glass is quiet again
}

TEST("notice: a note that lands after the notice takes the line back") {
  TempDir dir;
  Store store(dir.path);
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0007", "2026-11-21T14:02:03Z", 40,
                  NoteState::Sent));
  UiState ui;
  ui.begin(seed_for(), &store);

  ui.set_notice(sub::kNoAudioSaved, 2000, 5000);
  CHECK(ui.publish(2000, DirtyNotice, Refresh::Partial).draw);
  CHECK(std::string(ui.app().frame().home.sub) == "NO AUDIO SAVED");

  // A second note is committed while the line is up (a very short note, the
  // runtime's own commit): "nothing saved" would be a lie about that card, so the
  // publish drops the notice and draws the new note's LAST line instead.
  CHECK(make_note(store, "hg-69ffb4e9-20261121T140203-0008", "2026-11-21T14:05:00Z", 1,
                  NoteState::Sent));
  const Outcome committed = ui.publish(3000, static_cast<uint16_t>(DirtyQueue | DirtyList));
  CHECK(committed.draw);
  CHECK(std::string(ui.app().frame().home.sub) == "LAST 14:05");
  CHECK(ui.snapshot().notice[0] == '\0');
}

TEST("facts: a capture silences the band, and the band catches up after it") {
  TempDir dir;
  Store store(dir.path);
  UiState ui;
  ui.begin(battery_seed(84), &store);
  CHECK(!ui.set_facts(facts_at(84), 1000).draw);  // nothing moved; no clock minute

  // Recording: every fact is dropped, so the 5 s counter stays the only thing
  // that repaints the panel.
  CHECK(ui.set_capture(true, 0, false, 2000).draw);  // the screen hand-over is full
  Facts noisy = facts_at(40, false);
  noisy.bt = BtState::Advertising;
  noisy.card_free_bytes = 12345678;
  CHECK(!ui.set_facts(noisy, 3000).draw);
  CHECK(!ui.set_facts(noisy, 13000).draw);
  CHECK(ui.snapshot().battery_pct == 84);      // nothing was adopted mid-note
  CHECK(ui.snapshot().bt == BtState::Off);
  CHECK(!ui.tick(6000).draw);                  // and four seconds in is before the first mark

  // The capture ends: the next poll is the one that brings the band up to date.
  CHECK(ui.set_capture(false, 0, false, 15000).draw);
  CHECK(ui.screen() == Screen::Home);
  CHECK(ui.set_facts(noisy, 16000).draw);
  CHECK(ui.snapshot().battery_pct == 40);
  CHECK(ui.snapshot().bt == BtState::Advertising);
  CHECK(!ui.snapshot().clock_set);
  CHECK(std::string(build_home(ui.snapshot()).clock_msg) == "NO TIME");
}
