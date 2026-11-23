// See notes_app/ui_state.hpp.
#include "notes_app/ui_state.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hgp::notesui {
namespace {

// The flicker budget (t_3beb371a §5). The ADC moves a percent or two between
// reads with nothing happening, and every publish is a whole-panel waveform, so
// a fact has to earn its frame.
constexpr int kBatteryStepPct = 2;                          // ignore the 1 % noise
constexpr uint32_t kBatteryRateMs = 5u * 60u * 1000u;       // and no more than this often
constexpr uint64_t kCardFreeSlackBytes = 1ull << 20;        // 1 MiB: a write moves free space

}  // namespace

void UiState::begin(const Snapshot& seed, notes::Store* store) {
  store_ = store;
  snap_ = seed;
  snap_.low_power = seed.battery_pct >= 0 && seed.battery_pct <= 10 && !seed.external;
  load_counts();
  // The machine sees the counts before the first window is filled: Home's LAST
  // line reads the newest stub, and asking for it before begin() empty handed
  // answered {0,0}, so the window was skipped and the boot frame said BOOT TO
  // RECORD with a full card (QA D2).
  app_.begin(snap_);
  snap_.stub_first = snap_.stub_rows = 0;
  load_window();
  // The boot frame already carries the seed's battery, so the five-minute power
  // window starts at power-on rather than at the first poll.
  last_power_ms_ = 1;
}

void UiState::load_counts() {
  if (store_ == nullptr) {
    cache_ = Cache{};
    snap_.card = false;
    snap_.count_pending = snap_.count_sent = snap_.count_failed = 0;
    snap_.note_count = 0;
    snap_.list_truncated = false;
    snap_.stub_first = snap_.stub_rows = 0;
    return;
  }
  snap_.card = cache_.reload(*store_);
  snap_.count_pending = cache_.pending();
  snap_.count_sent = cache_.sent();
  snap_.count_failed = cache_.failed();
  snap_.note_count = static_cast<uint16_t>(cache_.note_count());
  snap_.list_truncated = cache_.truncated();
  // Whatever the count, the window is stale by construction: it is refilled
  // below, for the window the next frame will read.
  snap_.stub_first = snap_.stub_rows = 0;
}

void UiState::load_window() {
  // The app owns this answer: it is the layer that decides which stubs a frame
  // reads, so the caller cannot fill a window the renderer does not use.
  const Window want = app_.window();
  if (want.first == snap_.stub_first && want.rows == snap_.stub_rows) return;
  if (store_ == nullptr || !snap_.card) {
    snap_.stub_first = snap_.stub_rows = 0;
    return;
  }
  cache_.fill(*store_, snap_, want.first, want.rows);
}

void UiState::adopt_app_facts() {
  const Snapshot& live = app_.snapshot();
  snap_.recording = live.recording;
  snap_.record_elapsed_s = live.record_elapsed_s;
  // The arm for a tap during an upload is the screen machine's, not a fact of the
  // card: a publish that did not carry it back would drop the wait off the glass
  // and leave the counter counting from the tap.
  snap_.record_queued = live.record_queued;
  snap_.playing = live.playing;
  snap_.play_total_s = play_total_s_ != 0 ? play_total_s_ : live.play_total_s;
  snap_.play_pos_s = play_pos_s_;
}

Outcome UiState::publish(uint32_t now_ms, uint16_t dirty, Refresh at_least) {
  adopt_app_facts();
  snap_.upload_active = upload_active_;
  snap_.upload_failed = upload_failed_;
  snap_.upload_done = upload_done_;
  snap_.upload_total = upload_total_;
  load_counts();
  // A note that landed (or was deleted) after the notice was set makes it a lie
  // about the card: the line goes back to the counts. A discard never moves them,
  // so the notice it set survives the publish that carries it.
  if (notice_until_ms_ != 0 && snap_.notes_total() != notice_notes_) {
    notice_until_ms_ = 0;
    snap_.notice[0] = '\0';
  }
  load_window();
  Outcome out = app_.publish(snap_, dirty);
  if (out.refresh != Refresh::None || at_least != Refresh::None) {
    // The caller's own transition is the floor: a screen hand-over needs a full
    // frame even when the cache it carries is the one already on the glass.
    if (static_cast<uint8_t>(out.refresh) < static_cast<uint8_t>(at_least)) out.refresh = at_least;
    out.draw = true;
  }
  load_window();
  return out;
}

Outcome UiState::set_facts(const Facts& facts, uint32_t now_ms) {
  // A capture owns the glass (t_3beb371a §5): the 30 s counter is the only
  // repaint it allows, so nothing here is published or even adopted - the facts
  // stay in the caller's cache and the first poll after the stop compares them
  // with what is still drawn, which is what makes the band correct again without
  // a repaint in the middle of the note.
  if (capture_recording_ || snap_.recording) return Outcome{};

  char clock_text[6] = {};
  if (facts.clock.valid) format_clock(facts.clock, clock_text);

  uint16_t dirty = DirtyNone;
  if (facts.wifi_up != snap_.wifi_up) dirty |= DirtyLink;
  if (facts.bt != snap_.bt) dirty |= DirtyLink;  // both live in the band
  const bool had_clock = snap_.clock_text[0] != '\0';
  const bool has_clock = clock_text[0] != '\0';
  if (facts.clock_set != snap_.clock_set || had_clock != has_clock ||
      (has_clock && std::strcmp(clock_text, snap_.clock_text) != 0)) {
    dirty |= DirtyClock;
  }

  // The battery: a step of one percent is the ADC talking, not the cell. A step
  // of two or more draws, and so does a smaller one once five minutes have
  // passed, so the level never drifts far from the glass while nothing happens.
  const bool external_changed = facts.external != snap_.external;
  const bool pct_changed = facts.battery_pct != snap_.battery_pct;
  const int step = facts.battery_pct >= 0 && snap_.battery_pct >= 0
                       ? std::abs(static_cast<int>(facts.battery_pct) - static_cast<int>(snap_.battery_pct))
                       : 100;
  const bool rate_due = last_power_ms_ == 0 || now_ms - last_power_ms_ >= kBatteryRateMs;
  const bool low = facts.battery_pct >= 0 && facts.battery_pct <= 10 && !facts.external;
  const bool power_dirty = external_changed || low != snap_.low_power ||
                           (pct_changed && (step >= kBatteryStepPct || rate_due));

  const uint64_t free_now = facts.card_free_bytes, free_was = snap_.card_free_bytes;
  const uint64_t free_moved = free_now > free_was ? free_now - free_was : free_was - free_now;
  const bool about_dirty = free_moved >= kCardFreeSlackBytes;

  if (power_dirty) dirty |= DirtyPower;
  if (about_dirty) dirty |= DirtyAbout;

  snap_.wifi_up = facts.wifi_up;
  snap_.wifi_on = facts.wifi_on;
  snap_.clock_set = facts.clock_set;
  std::snprintf(snap_.clock_text, sizeof snap_.clock_text, "%s", clock_text);
  snap_.bt = facts.bt;
  if (power_dirty) {
    snap_.battery_pct = facts.battery_pct;
    snap_.external = facts.external;
    snap_.low_power = low;
    last_power_ms_ = now_ms == 0 ? 1 : now_ms;
  }
  if (about_dirty) snap_.card_free_bytes = facts.card_free_bytes;

  // The clock is a Home fact: a minute that turns over on any other screen is
  // adopted but not drawn, and the full frame that returns Home carries it.
  if ((dirty & DirtyClock) && app_.screen() != Screen::Home) dirty &= ~DirtyClock;
  if (dirty == DirtyNone) return Outcome{};
  return publish(now_ms, dirty, Refresh::Partial);
}

Outcome UiState::press(Key key, uint32_t now_ms) {
  // The card first: a key acts on what is on it now. This is the other half of
  // D1 - the press path never re-read the counts, so even a fresh snapshot would
  // have handed the machine yesterday's list.
  //
  // Except while a wipe or a format owns the volume (t_0070a731): the screen is
  // the keyless busy face, no count on it can change, and a card scan here waits
  // behind the worker's own removes - a press staged behind a 90 s wipe has no
  // business costing the console its 10 s answer. The counts are re-read by the
  // publish that ends the job.
  if (app_.screen() != Screen::Wiping) load_counts();
  adopt_app_facts();
  const Outcome synced = sync_app();
  Outcome out = app_.press(key, now_ms);
  merge(out, synced);  // a hero that moved under the press still needs its frame
  adopt_app_facts();
  load_window();
  // The machine acted on the snapshot sync_app() pushed before it moved, and
  // load_counts() had just zeroed that snapshot's window. Fill the window for
  // the screen the press landed on and hand it back: otherwise the frame this
  // press draws reads no rows - the list shows its title and footer over an
  // empty body (t_8c3e4900). sync_app() asks for no frame of its own; the press's
  // Outcome is the only one this path returns.
  sync_app();
  return out;
}

Outcome UiState::tick(uint32_t now_ms) {
  adopt_app_facts();
  const Outcome followed = app_.follow_capture(capture_recording_, capture_elapsed_s_, now_ms, capture_queued_);
  const Outcome synced = sync_app();
  Outcome out = app_.tick(now_ms);
  merge(out, synced);
  merge(out, followed);
  adopt_app_facts();
  // Home's notice is the one thing on the glass that lapses on its own: when its
  // few seconds are up the counts line comes back, on Home and only on Home (a
  // notice that lapsed behind another screen is simply gone from the next Home
  // frame).
  if (notice_until_ms_ != 0 && static_cast<int32_t>(now_ms - notice_until_ms_) >= 0) {
    notice_until_ms_ = 0;
    snap_.notice[0] = '\0';
    if (app_.screen() == Screen::Home) {
      merge(out, publish(now_ms, DirtyNotice, Refresh::Partial));
    } else {
      sync_app();
    }
  }
  return out;
}

Outcome UiState::set_capture(bool recording, uint32_t elapsed_s, bool queued, uint32_t now_ms,
                            uint32_t cap_s, uint64_t cap_bytes) {
  capture_recording_ = recording;
  capture_queued_ = queued;
  capture_elapsed_s_ = elapsed_s;
  if (recording) {
    // A new capture is the news now, and the last stop's line would be a stale
    // complaint over a note that is being made.
    notice_until_ms_ = 0;
    snap_.notice[0] = '\0';
  }
  // The capture's own cap, not the setting's: the face says `OF 1:30·2.1G` when
  // the card is what limits the note, and the screen's cap stop lands on the
  // same second the runtime stops the file. Written before the app is asked, so
  // the frame this returns already carries it.
  if (cap_s != 0) snap_.note_max_s = cap_s;
  snap_.record_cap_bytes = cap_bytes;
  app_.set_capture_cap(cap_s, cap_bytes);
  adopt_app_facts();
  Outcome out = app_.follow_capture(recording, elapsed_s, now_ms, queued);
  adopt_app_facts();
  return out;
}

Outcome UiState::decline(Effect effect) {
  Outcome out = app_.decline(effect);
  adopt_app_facts();
  return out;
}

Outcome UiState::pairing_ask(uint32_t passkey) { return app_.pairing_ask(passkey); }

Outcome UiState::pairing_answer(bool allow) { return app_.pairing_answer(allow); }

// The card job's two moves (t_0070a731). No card work and no snapshot rebuild:
// the counts the job leaves are read by the publish the caller makes when it is
// over, and the face itself only needs the runtime's numbers.
Outcome UiState::set_card_job(bool format, uint32_t done, uint32_t total) {
  return app_.card_job_progress(format, done, total);
}

Outcome UiState::card_job_finished() { return app_.card_job_finished(); }

// The Phone screen's window is the port's answer, not the card's: hand it to the
// machine, which decides whether the glass moves.
Outcome UiState::set_phone(const PhoneStatus& status, uint32_t now_ms) { return app_.set_phone(status, now_ms); }

Outcome UiState::playback_finished() {
  adopt_app_facts();
  const Outcome synced = sync_app();
  Outcome out = app_.playback_finished();
  merge(out, synced);
  adopt_app_facts();
  return out;
}

Outcome UiState::sync_app() { return app_.publish(snap_, DirtyNone); }

void UiState::merge(Outcome& out, const Outcome& more) {
  if (static_cast<uint8_t>(more.refresh) > static_cast<uint8_t>(out.refresh)) out.refresh = more.refresh;
  out.draw = out.draw || more.draw;
}

void UiState::set_upload(bool active, uint8_t done, uint8_t total) {
  upload_active_ = active;
  upload_done_ = done;
  upload_total_ = total;
  if (active) upload_failed_ = false;
}

void UiState::set_upload_failed() {
  upload_active_ = false;
  upload_failed_ = true;
}

void UiState::set_play(uint32_t pos_s, uint32_t total_s) {
  play_pos_s_ = pos_s;
  if (total_s != 0) play_total_s_ = total_s;
}

bool UiState::set_busy(std::string_view reason) {
  const size_t n = std::min(reason.size(), sizeof snap_.busy - 1);
  const bool same = n == std::strlen(snap_.busy) && std::memcmp(snap_.busy, reason.data(), n) == 0;
  if (same) return false;
  std::memcpy(snap_.busy, reason.data(), n);
  snap_.busy[n] = '\0';
  return true;
}

void UiState::set_notice(std::string_view text, uint32_t now_ms, uint32_t ttl_ms) {
  const size_t n = std::min(text.size(), sizeof snap_.notice - 1);
  std::memcpy(snap_.notice, text.data(), n);
  snap_.notice[n] = '\0';
  // A zero ttl (or an empty line) means "nothing to say": no deadline, no line,
  // so a caller that has nothing to explain costs no frame.
  notice_until_ms_ = (ttl_ms != 0 && n != 0) ? (now_ms == 0 ? 1 : now_ms) + ttl_ms : 0;
  if (notice_until_ms_ == 0) snap_.notice[0] = '\0';
  // The counts as they are now: a note that lands later takes the line back.
  notice_notes_ = snap_.notes_total();
}

}  // namespace hgp::notesui
