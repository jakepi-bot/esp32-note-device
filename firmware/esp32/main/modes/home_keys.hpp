// The notes device's two physical keys as actions (no GPIO, no FreeRTOS: the
// host tests drive it, firmware/tests/test_epaper_keys.cpp; port-level wiring is
// modes/mode_router.cpp).
//
// The mapping the product asks for:
//   PWR  (the power key)          a short press talks to Hermes: it starts a live
//                                 turn, and the next short press ends it. A long
//                                 hold is the latch's own power-off, so it acts
//                                 nothing here, which is why the menu cannot be a
//                                 chord: holding PWR switches the board off.
//   BOOT (the second key)         a tap starts or stops a note. Held past
//                                 `cancel_hold_ms` it does the one thing there is
//                                 to do with what is on the glass: drops the
//                                 recording, leaves the menu, closes a live turn,
//                                 or, on the home screen, opens the menu (the
//                                 core's settings list). BOOT never talks.
//
// Two keys, three jobs, so the held BOOT is contextual and `route_key()` below is
// the whole rule. In the menu a BOOT tap moves to the next item and a PWR tap
// changes it (the core's CANCEL and TALK), which is the same two-key grammar the
// other boards use.
//
// There is no brightness key and none is needed: this panel has no backlight, so
// `set_backlight` and the core's brightness setting do nothing on it. The
// request's "brightness button" is this second key, BOOT.
//
// The desk unit has no touch glass (2026-11-21: the round-2 I2C probe was
// right), so both keys are the whole input surface and the screen has no strip.
#pragma once

#include <cstdint>

namespace hgp {

// What the port must do after one key edge. Idle presses resolve to None.
enum class HomeAction : uint8_t {
  None,
  TalkToggle,  // PWR short: start a live turn, or end the one in progress
  NoteToggle,  // notes mode: start a note, or finish the one recording
  Hold,        // BOOT held: drop the recording, leave the menu, close the turn,
               // or open the menu (what it is depends on the screen; see route_key)
};

// What a key action answers while a phone is asking to pair. Nothing else on
// the device can answer it, so the ask owns both keys while it shows: BOOT (a
// tap) allows, PWR (a short press) refuses. Twenty seconds with neither is the
// core's own timeout, which refuses (App::kPhoneAskMs).
enum class AskAnswer : uint8_t { None, Allow, Deny };

// The answer a resolved key action gives a pairing ask, if any.
AskAnswer pairing_ask_answer(HomeAction action);

// What is on the glass, as far as the keys are concerned. The port fills this in
// from the app and the notes mode; nothing here knows about either.
struct KeyContext {
  bool phone_ask = false;       // a phone is asking to pair: it owns both keys
  bool menu_open = false;       // the settings list is showing
  bool note_recording = false;  // a note is being captured
  bool turn_or_overlay = false; // a live turn, a reply, a prompt or a card is up
};

// What one key action becomes. The router only carries these out, so the rules
// that matter (and the surprising ones, like "held BOOT opens the menu only at
// home") are testable without a board.
enum class KeyRoute : uint8_t {
  None,
  AskAllow,      // BOOT tap while a phone asks
  AskDeny,       // PWR tap while a phone asks
  TalkToggle,    // PWR short, no menu: start or end a live turn
  NoteToggle,    // BOOT tap, no menu or recording: start or stop a note
  MenuTapNext,   // BOOT tap in the menu: next item (the core's CANCEL)
  MenuTapChange, // PWR tap in the menu: change the item (the core's TALK)
  CancelNote,    // BOOT held while a note records: drop it
  MenuClose,     // BOOT held in the menu: back to what was there before
  CancelTurn,    // BOOT held with a turn or an overlay up: close it
  MenuOpen,      // BOOT held on the home screen: the settings list
};

// The one rule: a gesture, plus what is on the glass, is exactly one route.
KeyRoute route_key(HomeAction action, const KeyContext& context);


class HomeKeys {
 public:
  struct Config {
    // A press longer than this is no longer a tap: the latch powers the board off
    // at 2.5 s, so a press this long is on its way there and must not act.
    uint32_t tap_max_ms = 1500;
    // BOOT held this long acts instead of recording. The same 2 s the rest of the
    // firmware uses for a long press (ButtonConfig.talk_hold_cancel_ms).
    uint32_t cancel_hold_ms = 2000;
  };

  void begin(const Config& cfg) { cfg_ = cfg; }
  const Config& config() const { return cfg_; }

  // The power key. A press only starts the clock; the release decides, so a hold
  // meant for the power-off never flashes a screen first.
  HomeAction power_press(uint32_t now_ms);
  HomeAction power_release(uint32_t now_ms);

  // The record/cancel key.
  HomeAction key_press(uint32_t now_ms);
  HomeAction key_release(uint32_t now_ms);
  // While the key is down: fires Hold once when the press passes
  // `cancel_hold_ms`, so the recording is dropped (or the menu opens) while the
  // key is still held.
  HomeAction tick(uint32_t now_ms);

  bool power_down() const { return power_down_; }
  bool key_down() const { return key_down_; }
  bool cancelling() const { return cancelling_; }

 private:
  Config cfg_{};
  bool power_down_ = false;
  uint32_t power_at_ms_ = 0;
  bool key_down_ = false;
  uint32_t key_at_ms_ = 0;
  bool cancelling_ = false;  // this press passed the hold
};

}  // namespace hgp
