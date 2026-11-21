// The notes device's two physical keys as actions (no GPIO, no FreeRTOS: the
// host tests drive it, firmware/tests/test_epaper_keys.cpp; port-level wiring is
// modes/mode_router.cpp).
//
// The mapping the product asks for:
//   PWR  (the power key)          a short press is Home: back to the resting face,
//                                 whatever is open. A long hold is still the latch's
//                                 own power-off, so it acts nothing here.
//   BOOT (the second key)         a tap starts or stops a note. Held, it talks, so a
//                                 unit whose glass has no touch controller can still
//                                 reach the live mode; on a touch unit the home
//                                 screen's TALK button does that instead (`hold_talks`
//                                 false, and a long hold then acts nothing so a slow
//                                 tap never records).
//
// There is no brightness key and none is needed: this panel has no backlight, so
// `set_backlight` and the core's brightness setting do nothing on it. The request's
// "brightness button" is this second key, BOOT.
#pragma once

#include <cstdint>

namespace hgp {

// What the port must do after one key edge. Idle presses resolve to None.
enum class HomeAction : uint8_t {
  None,
  Home,        // go_home(): the resting face, nothing open
  NoteToggle,  // notes mode: start a note, or finish the one recording
  TalkStart,   // hold-to-talk began: on_button(Talk, true)
  TalkStop,    // and ended: on_button(Talk, false)
};

class HomeKeys {
 public:
  struct Config {
    // A press held this long talks instead of recording a note. Short enough that
    // a note tap still feels like a tap, long enough not to fire on a slow one.
    uint32_t talk_hold_ms = 700;
    // A press longer than this is no longer a tap: the latch powers the board off
    // at 2.5 s, so a press this long is on its way there and must not act.
    uint32_t tap_max_ms = 1500;
    // False when the glass carries a touchscreen and the home screen's TALK button
    // starts the live mode: holding the key then does nothing.
    bool hold_talks = true;
  };

  void begin(const Config& cfg) { cfg_ = cfg; }
  const Config& config() const { return cfg_; }

  // The power key. A press only starts the clock; the release decides, so a hold
  // meant for the power-off never flashes the home screen first.
  HomeAction power_press(uint32_t now_ms);
  HomeAction power_release(uint32_t now_ms);

  // The record/talk key.
  HomeAction key_press(uint32_t now_ms);
  HomeAction key_release(uint32_t now_ms);
  // While the key is down: fires TalkStart once when the press passes
  // `talk_hold_ms`, so talking starts on the hold rather than on the lift.
  HomeAction tick(uint32_t now_ms);

  bool power_down() const { return power_down_; }
  bool key_down() const { return key_down_; }
  bool talking() const { return talking_; }

 private:
  Config cfg_{};
  bool power_down_ = false;
  uint32_t power_at_ms_ = 0;
  bool key_down_ = false;
  uint32_t key_at_ms_ = 0;
  bool talking_ = false;  // this press passed the hold and is talking now
};

}  // namespace hgp
