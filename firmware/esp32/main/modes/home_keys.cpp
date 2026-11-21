// See modes/home_keys.hpp.
#include "modes/home_keys.hpp"

namespace hgp {

AskAnswer pairing_ask_answer(HomeAction action) {
  switch (action) {
    case HomeAction::NoteToggle: return AskAnswer::Allow;  // BOOT
    case HomeAction::TalkToggle: return AskAnswer::Deny;   // PWR
    // A hold is not an answer: it is the gesture that opens the menu or closes
    // whatever is open, and a pairing ask must not be allowed by a press that
    // was held too long.
    case HomeAction::Hold:
    case HomeAction::None: return AskAnswer::None;
  }
  return AskAnswer::None;
}

KeyRoute route_key(HomeAction action, const KeyContext& context) {
  if (action == HomeAction::None) return KeyRoute::None;
  // The pairing question owns both keys while it is on the glass: nothing else
  // may open the microphone, the card or the menu behind it.
  if (context.phone_ask) {
    switch (pairing_ask_answer(action)) {
      case AskAnswer::Allow: return KeyRoute::AskAllow;
      case AskAnswer::Deny: return KeyRoute::AskDeny;
      case AskAnswer::None: return KeyRoute::None;
    }
  }
  switch (action) {
    case HomeAction::TalkToggle:
      return context.menu_open ? KeyRoute::MenuTapChange : KeyRoute::TalkToggle;
    case HomeAction::NoteToggle:
      return context.menu_open ? KeyRoute::MenuTapNext : KeyRoute::NoteToggle;
    case HomeAction::Hold:
      // One held key, four jobs, in the order that loses nothing: a recording in
      // progress is the most valuable thing on the device, then the menu that is
      // open, then a turn or a card somebody may be reading. Only a home screen
      // with none of those has nothing to close, and that is where the menu is.
      if (context.note_recording) return KeyRoute::CancelNote;
      if (context.menu_open) return KeyRoute::MenuClose;
      if (context.turn_or_overlay) return KeyRoute::CancelTurn;
      return KeyRoute::MenuOpen;
    case HomeAction::None: break;
  }
  return KeyRoute::None;
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
  return HomeAction::Hold;
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
