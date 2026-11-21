// See modes/auto_home.hpp.
#include "modes/auto_home.hpp"

namespace hgp {

bool auto_home_face(hg::Screen screen) {
  switch (screen) {
    // The faces modes/notes_screen.cpp paints: the device is resting.
    case hg::Screen::Ready:
    case hg::Screen::Offline:
    case hg::Screen::LowBattery:
    case hg::Screen::Charging:
    case hg::Screen::Charged: return true;
    // Pairing is the code screen: it is waiting for a phone, not resting.
    default: return false;
  }
}

bool AutoHome::tick(uint32_t now_ms, const Inputs& in) {
  // Nothing to return from, nothing to interrupt, or nobody looking: the clock
  // starts over, so the minute only ever counts quiet time on another screen.
  if (in.at_home || in.dozing || in.busy) {
    last_ms_ = now_ms;
    return false;
  }
  if (now_ms - last_ms_ < kIdleMs) return false;
  last_ms_ = now_ms;  // one return home per quiet minute, not one per pass
  return true;
}

}  // namespace hgp
