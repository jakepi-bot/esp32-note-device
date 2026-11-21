// See modes/home_keys.hpp.
#include "modes/home_keys.hpp"

namespace hgp {

HomeAction HomeKeys::power_press(uint32_t now_ms) {
  power_down_ = true;
  power_at_ms_ = now_ms;
  return HomeAction::None;
}

HomeAction HomeKeys::power_release(uint32_t now_ms) {
  if (!power_down_) return HomeAction::None;
  power_down_ = false;
  // A press this long is the latch's power-off, not a home press.
  return now_ms - power_at_ms_ <= cfg_.tap_max_ms ? HomeAction::Home : HomeAction::None;
}

HomeAction HomeKeys::key_press(uint32_t now_ms) {
  key_down_ = true;
  key_at_ms_ = now_ms;
  talking_ = false;
  return HomeAction::None;
}

HomeAction HomeKeys::tick(uint32_t now_ms) {
  if (!key_down_ || talking_ || !cfg_.hold_talks) return HomeAction::None;
  if (now_ms - key_at_ms_ < cfg_.talk_hold_ms) return HomeAction::None;
  talking_ = true;
  return HomeAction::TalkStart;
}

HomeAction HomeKeys::key_release(uint32_t now_ms) {
  if (!key_down_) return HomeAction::None;
  key_down_ = false;
  if (talking_) {
    talking_ = false;
    return HomeAction::TalkStop;
  }
  // A tap, and only a tap: a press on its way to the latch (or to a hold that
  // never talked because the glass has a touchscreen) records nothing.
  return now_ms - key_at_ms_ <= cfg_.tap_max_ms ? HomeAction::NoteToggle : HomeAction::None;
}

}  // namespace hgp
