// The adapter's refresh policy, without a panel: the cached snapshot the screens
// draw from, and the rule that keeps it current.
//
// ui_device.cpp owns the ports and the glass; this owns the data. The split is
// deliberate. The QA pass on the notes rewrite (t_5baa6311, defect D1) found the
// snapshot built once at boot and never rebuilt - the only publishers were
// callbacks nothing called - so a note recorded after power-on never reached the
// glass and the Home band and hero froze. Nothing in ctest could see it, because
// every host case fed a hand-built Snapshot straight into ScreenApp. Here the
// same policy runs against a real card in a temp directory
// (tests/test_notes_refresh.cpp), so the event-on-the-card -> new-snapshot rule
// is a test rather than a comment.
#pragma once

#include <cstdint>

#include "notes/notes_store.hpp"
#include "notes_app/ui_app.hpp"
#include "notes_app/ui_cache.hpp"
#include "notes_app/ui_view.hpp"

namespace hgp::notesui {

// What the ports answer that the screens cache. Read on an event, never while a
// frame is drawn (docs/notes-ui-spec.md §15.4).
struct Facts {
  bool wifi_up = false;        // the station holds an association
  bool clock_set = true;       // the clock has been set at least once
  Stamp clock;                 // the wall clock's local fields; `valid` is the answer
  BtState bt = BtState::Off;   // the radio's window, off outside the Phone screen
  uint64_t card_free_bytes = 0;
  int8_t battery_pct = -1;     // -1 unknown
  bool external = false;       // on a cable
};

class UiState {
 public:
  // Boot. `seed` carries what the ports already know (firmware, device name, the
  // recording cap and the first Facts). The card is read once, the screen
  // machine is given the counts, and only then is the first window filled: that
  // order is what keeps the boot frame's sub-line honest (QA D2 - it read
  // "BOOT TO RECORD" with 58 notes on the card).
  void begin(const Snapshot& seed, notes::Store* store);

  // Something the screens cannot see moved: a note was committed, deleted,
  // moved or pruned, an upload stepped, the card came or went. Re-reads the card
  // and asks the screen machine what that does to the glass. `at_least` is the
  // refresh class the caller's own transition owes (a screen hand-over is always
  // full), so one publish can serve both.
  Outcome publish(uint32_t now_ms, uint16_t dirty, Refresh at_least = Refresh::None);

  // The link, the clock, the card's free space and the battery: compared with
  // the snapshot, so a fact that moved between events still reaches the glass
  // and one that did not costs no frame. The comparison is where the flicker
  // budget lives (t_3beb371a §5): a 1 % battery step from the noisy ADC is not a
  // change, and nothing at all is published while a capture is running.
  Outcome set_facts(const Facts& facts, uint32_t now_ms);

  // A key. The card is read *before* the screen machine acts, so a press works
  // on the notes that exist now and not on the ones that existed at the last
  // publish (QA D1: a key could not reach a note recorded since boot).
  Outcome press(Key key, uint32_t now_ms);

  Outcome tick(uint32_t now_ms);
  // The runtime's own capture state, handed over before a tick or a publish.
  // The screens follow it (ScreenApp::follow_capture): a capture that ended
  // without a key leaves the recording face, one no key started enters it, and a
  // tap the runtime kept for an upload in flight draws the queued face.
  Outcome set_capture(bool recording, uint32_t elapsed_s, bool queued, uint32_t now_ms);
  // The runtime declined an effect the screens had acted on: put back the move
  // it was for (ScreenApp::decline) and keep the snapshot honest.
  Outcome decline(Effect effect);
  Outcome pairing_ask(uint32_t passkey);
  Outcome pairing_answer(bool allow);
  Outcome playback_finished();
  // The Phone screen's window, straight from the port: no card work, so it is
  // the screens' own answer and nothing here has to re-read anything.
  Outcome set_phone(const PhoneStatus& status, uint32_t now_ms);

  // Upload progress and the player's position belong to their owners; they hand
  // them over here so the next frame draws them.
  void set_upload(bool active, uint8_t done, uint8_t total);
  void set_upload_failed();
  void set_play(uint32_t pos_s, uint32_t total_s);

  const Snapshot& snapshot() const { return snap_; }
  Screen screen() const { return app_.screen(); }
  const ScreenApp& app() const { return app_; }
  Window window() const { return app_.window(); }
  const Cache& cache() const { return cache_; }
  notes::Store* store() const { return store_; }

 private:
  // The screen machine owns the recording counter and the player's totals; take
  // them back before a publish, so rebuilding the snapshot cannot reset what the
  // glass is drawing.
  void adopt_app_facts();
  // Hand the machine the current snapshot without asking for a frame: it reads
  // the counts, the cursor and the position from its own copy of it.
  Outcome sync_app();
  // `out` keeps the harsher of the two refresh classes and draws if either did.
  static void merge(Outcome& out, const Outcome& more);
  void load_counts();
  void load_window();

  Snapshot snap_;
  ScreenApp app_;
  Cache cache_;
  notes::Store* store_ = nullptr;
  bool upload_active_ = false;
  bool upload_failed_ = false;
  uint8_t upload_done_ = 0, upload_total_ = 0;
  uint32_t play_pos_s_ = 0;
  uint32_t play_total_s_ = 0;  // 0: the machine's own answer
  // The runtime's last answer about the capture. Nothing here decides whether a
  // recording is running: this is what the device says, and the screens follow.
  bool capture_recording_ = false;
  bool capture_queued_ = false;
  uint32_t capture_elapsed_s_ = 0;
  // The last power fact put on the glass: a 1 % ADC step waits until the step
  // is 2 % or five minutes have passed (t_3beb371a §5).
  uint32_t last_power_ms_ = 0;
};

}  // namespace hgp::notesui
