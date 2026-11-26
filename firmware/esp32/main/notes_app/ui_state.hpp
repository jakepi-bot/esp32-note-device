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
#include <string_view>

#include "notes/notes_store.hpp"
#include "notes_app/ui_app.hpp"
#include "notes_app/ui_cache.hpp"
#include "notes_app/ui_peers.hpp"
#include "notes_app/ui_view.hpp"

namespace hgp::notesui {

// What the ports answer that the screens cache. Read on an event, never while a
// frame is drawn (docs/notes-ui-spec.md §15.4).
struct Facts {
  bool wifi_on = false;        // the station's radio is up (associated or not)
  bool wifi_up = false;        // the station holds an association
  bool clock_set = true;       // the clock has been set at least once
  // No wall-clock fields here: the band only needs the answer above now that the
  // time is no longer drawn (Jake, 2026-11-26). Notes and uploads still read the
  // system clock themselves; this cache is not their source.
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
  // "BOOT TO RECORD" with 58 notes on the card). `bonds` is the radio's table, for
  // the Unpair screen; null on a build without Bluetooth.
  void begin(const Snapshot& seed, notes::Store* store, BondPort* bonds = nullptr);

  // Something the screens cannot see moved: a note was committed, deleted,
  // moved or pruned, an upload stepped, the card came or went. Re-reads the card
  // and asks the screen machine what that does to the glass. `at_least` is the
  // refresh class the caller's own transition owes (a screen hand-over is the
  // class `ScreenApp::handover_frame()` names), so one publish can serve both.
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
  // The Unpair confirm's hold: forget the phone at `index` in the snapshot's
  // table (one erase), then re-read the table and publish, so the list the hold
  // came back to is one row shorter and the Phone screen's count agrees. The
  // screens have already refused a busy device; a store that refuses changes
  // nothing and asks for no frame.
  Outcome unpair(uint8_t index, uint32_t now_ms);
  // The runtime's own capture state, handed over before a tick or a publish.
  // The screens follow it (ScreenApp::follow_capture): a capture that ended
  // without a key leaves the recording face, one no key started enters it, and a
  // tap the runtime kept for an upload in flight draws the queued face.
  // `cap_s` and `cap_bytes` are what the capture actually started with (the
  // setting, the contract ceiling and the card's room, whichever is smallest):
  // the face's cap line and the point the screen stops the capture at. 0 leaves
  // the snapshot's own cap in place.
  Outcome set_capture(bool recording, uint32_t elapsed_s, bool queued, uint32_t now_ms,
                      uint32_t cap_s = 0, uint64_t cap_bytes = 0);
  // The runtime declined an effect the screens had acted on: put back the move
  // it was for (ScreenApp::decline) and keep the snapshot honest.
  Outcome decline(Effect effect);
  Outcome pairing_ask(uint32_t passkey, const uint8_t* peer = nullptr);
  Outcome pairing_answer(bool allow);
  // The port says a phone's bond is made: the ask that phone put on the glass
  // leaves by itself (ScreenApp::pairing_paired). Host-testable here, not in the
  // device adapter - the rule is the screens'.
  Outcome pairing_paired(const uint8_t* peer = nullptr);
  Outcome playback_finished();
  // The wipe/format the runtime is running (t_0070a731). The machine owns the
  // face and the counts; these are the two moves the app task makes on it.
  Outcome set_card_job(bool format, uint32_t done, uint32_t total);
  Outcome card_job_finished();
  // The Phone screen's window, straight from the port: no card work, so it is
  // the screens' own answer and nothing here has to re-read anything.
  Outcome set_phone(const PhoneStatus& status, uint32_t now_ms);

  // Upload progress and the player's position belong to their owners; they hand
  // them over here so the next frame draws them.
  void set_upload(bool active, uint8_t done, uint8_t total);
  void set_upload_failed();
  void set_play(uint32_t pos_s, uint32_t total_s);

  // Why the device cannot be emptied right now, in the runtime's words (empty
  // when it is idle). Adopted here, not asked for by the frame: the two confirms
  // draw it, and the busy guard reads it. Returns whether the line moved.
  bool set_busy(std::string_view reason);

  // A one-line reason for Home's slot under the hero (a capture the stop
  // discarded for holding no audio), for `ttl_ms` and then no longer: the
  // counts line comes back when it lapses, which tick() watches for. It is
  // adopted, not drawn - the caller publishes (`DirtyNotice`) when the glass may
  // see it, the way the other facts above are handed over. A capture that starts
  // or a card that moves drops it early: it explains the last stop, and a new
  // note would make it a lie.
  void set_notice(std::string_view text, uint32_t now_ms, uint32_t ttl_ms);

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
  // The Unpair screen's table, on the event that opens or returns to it: the
  // radio's bond store is a fact like the card's counts, so it is read here and
  // never from the draw path. True when it moved.
  bool refresh_peers();

  Snapshot snap_;
  ScreenApp app_;
  Cache cache_;
  notes::Store* store_ = nullptr;
  BondPort* bonds_ = nullptr;
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
  // Home's notice: the ms it lapses at (0: none) and the card's total when it
  // was set, so a note that lands after it takes the line back (a saved note is
  // not a discarded one).
  uint32_t notice_until_ms_ = 0;
  uint32_t notice_notes_ = 0;
};

}  // namespace hgp::notesui
