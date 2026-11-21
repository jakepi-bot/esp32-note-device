// The talk key's long press. This board has no cancel button, so holding BOOT
// past a threshold taps CANCEL for it (integration spec section 6.1); the
// threshold is board data (`ButtonConfig.talk_hold_cancel_ms`, 0 disables it),
// so every other board's buttons behave exactly as before.
//
// Hardware-free: port_buttons.cpp feeds it the debounced edges, and the host
// tests drive it directly (firmware/tests/test_epaper_live_mode.cpp).
#pragma once

#include <cstdint>

#include "hg/ui.hpp"

namespace hgp {

// Whether a press that found this on screen had something to cancel: a turn in
// flight, or an overlay or question that took the screen. A press from the
// idle screen starts a fresh recording, and holding it is talk however long
// it lasts — arming there would cut every utterance past the threshold.
bool hold_cancel_armed(bool in_turn, hg::Screen screen);

// One talk key's long-press state. Armed by press(), fires once per hold.
class TalkHold {
 public:
  void begin(uint32_t threshold_ms) { threshold_ms_ = threshold_ms; }

  void press(uint32_t now_ms, bool armed) {
    if (!threshold_ms_ || !armed) return;
    down_at_ms_ = now_ms;
    armed_ = true;
    fired_ = false;
  }

  void release() { armed_ = fired_ = false; }

  // True on the first poll where the armed hold crosses the threshold, and
  // never twice for the same hold.
  bool fired(uint32_t now_ms) {
    if (!armed_ || fired_ || now_ms - down_at_ms_ < threshold_ms_) return false;
    fired_ = true;
    return true;
  }

 private:
  uint32_t threshold_ms_ = 0, down_at_ms_ = 0;
  bool armed_ = false, fired_ = false;
};

inline bool hold_cancel_armed(bool in_turn, hg::Screen screen) {
  if (in_turn) return true;
  switch (screen) {
    case hg::Screen::Widget:
    case hg::Screen::Profiles:
    case hg::Screen::Dashboard:
    case hg::Screen::Card:
    case hg::Screen::Image:
    case hg::Screen::Prompt:
    case hg::Screen::Choice: return true;
    default: return false;
  }
}

}  // namespace hgp
