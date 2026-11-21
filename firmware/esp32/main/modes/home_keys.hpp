// The notes device's two physical keys as actions (no GPIO, no FreeRTOS: the
// host tests drive it, firmware/tests/test_epaper_keys.cpp; port-level wiring is
// modes/mode_router.cpp).
//
// The mapping the product asks for:
//   PWR  (the power key)          a short press talks to Hermes: it starts a live
//                                 turn, and the next short press ends it. A long
//                                 hold is the latch's own power-off, so it acts
//                                 nothing here.
//   BOOT (the second key)         a tap starts or stops a note. Held past
//                                 `cancel_hold_ms` it cancels: a note being
//                                 recorded is dropped, and a live turn or an
//                                 overlay that took the screen is closed. BOOT
//                                 never talks.
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
  Cancel,      // BOOT held: drop the recording, or close what took the screen
};

// What a key action answers while a phone is asking to pair. Nothing else on
// the device can answer it, so the ask owns both keys while it shows: BOOT (a
// tap) allows, PWR (a short press) refuses. Twenty seconds with neither is the
// core's own timeout, which refuses (App::kPhoneAskMs).
enum class AskAnswer : uint8_t { None, Allow, Deny };

// The answer a resolved key action gives a pairing ask, if any.
AskAnswer pairing_ask_answer(HomeAction action);

class HomeKeys {
 public:
  struct Config {
    // A press longer than this is no longer a tap: the latch powers the board off
    // at 2.5 s, so a press this long is on its way there and must not act.
    uint32_t tap_max_ms = 1500;
    // BOOT held this long cancels instead of recording. The same 2 s the rest of
    // the firmware uses for a long press (ButtonConfig.talk_hold_cancel_ms).
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
  // While the key is down: fires Cancel once when the press passes
  // `cancel_hold_ms`, so the recording is dropped while the key is still held.
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
