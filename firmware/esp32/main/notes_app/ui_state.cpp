// See notes_app/ui_state.hpp.
#include "notes_app/ui_state.hpp"

#include <algorithm>

namespace hgp::notesui {

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
  uint16_t dirty = DirtyNone;
  if (facts.wifi_up != snap_.wifi_up) dirty |= DirtyLink;
  if (facts.clock_set != snap_.clock_set) dirty |= DirtyClock;
  if (facts.card_free_bytes != snap_.card_free_bytes) dirty |= DirtyAbout;
  const bool low = facts.battery_pct >= 0 && facts.battery_pct <= 10 && !facts.external;
  if (facts.battery_pct != snap_.battery_pct || facts.external != snap_.external || low != snap_.low_power)
    dirty |= DirtyPower;

  snap_.wifi_up = facts.wifi_up;
  snap_.clock_set = facts.clock_set;
  snap_.card_free_bytes = facts.card_free_bytes;
  snap_.battery_pct = facts.battery_pct;
  snap_.external = facts.external;
  snap_.low_power = low;
  if (dirty == DirtyNone) return Outcome{};
  return publish(now_ms, dirty, Refresh::Partial);
}

Outcome UiState::press(Key key, uint32_t now_ms) {
  // The card first: a key acts on what is on it now. This is the other half of
  // D1 - the press path never re-read the counts, so even a fresh snapshot would
  // have handed the machine yesterday's list.
  load_counts();
  adopt_app_facts();
  const Outcome synced = sync_app();
  Outcome out = app_.press(key, now_ms);
  merge(out, synced);  // a hero that moved under the press still needs its frame
  adopt_app_facts();
  load_window();
  return out;
}

Outcome UiState::tick(uint32_t now_ms) {
  adopt_app_facts();
  const Outcome followed = app_.follow_capture(capture_recording_, capture_elapsed_s_, now_ms);
  const Outcome synced = sync_app();
  Outcome out = app_.tick(now_ms);
  merge(out, synced);
  merge(out, followed);
  adopt_app_facts();
  return out;
}

Outcome UiState::set_capture(bool recording, uint32_t elapsed_s, uint32_t now_ms) {
  capture_recording_ = recording;
  capture_elapsed_s_ = elapsed_s;
  adopt_app_facts();
  Outcome out = app_.follow_capture(recording, elapsed_s, now_ms);
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

}  // namespace hgp::notesui
