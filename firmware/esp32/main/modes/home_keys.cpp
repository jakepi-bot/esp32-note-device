// See modes/home_keys.hpp.
#include "modes/home_keys.hpp"

namespace hgp {

AskAnswer pairing_ask_answer(HomeAction action) {
  switch (action) {
    case HomeAction::NoteToggle: return AskAnswer::Allow;  // BOOT
    case HomeAction::TalkToggle: return AskAnswer::Deny;   // PWR
    // A hold is not an answer: it is the gesture that cancels whatever is open,
    // and a pairing ask must not be allowed by a press that was held too long.
    case HomeAction::Cancel:
    case HomeAction::None: return AskAnswer::None;
  }
  return AskAnswer::None;
}

HomeAction HomeKeys::power_press(uint32_t now_ms) {
  power_down_ = true;
  power_at_ms_ = now_ms;
  return HomeAction::None;
}

HomeAction HomeKeys::power_release(uint32_t now_ms) {
  if (!power_down_) return HomeAction::None;
  power_down_ = false;
  // A press this long is the latch's power-off, not a talk press.
  return now_ms - power_at_ms_ <= cfg_.tap_max_ms ? HomeAction::TalkToggle : HomeAction::None;
}

HomeAction HomeKeys::key_press(uint32_t now_ms) {
  key_down_ = true;
  key_at_ms_ = now_ms;
  cancelling_ = false;
  return HomeAction::None;
}

HomeAction HomeKeys::tick(uint32_t now_ms) {
  if (!key_down_ || cancelling_) return HomeAction::None;
  if (now_ms - key_at_ms_ < cfg_.cancel_hold_ms) return HomeAction::None;
  cancelling_ = true;
  return HomeAction::Cancel;
}

HomeAction HomeKeys::key_release(uint32_t now_ms) {
  if (!key_down_) return HomeAction::None;
  key_down_ = false;
  if (cancelling_) {
    cancelling_ = false;
    return HomeAction::None;  // the hold already acted; the lift is not a tap
  }
  // A tap, and only a tap: a press on its way to the latch (or held long enough
  // to be the cancel) records nothing.
  return now_ms - key_at_ms_ <= cfg_.tap_max_ms ? HomeAction::NoteToggle : HomeAction::None;
}

}  // namespace hgp
