// Auto-home: the notes device has no home button, so after a minute with
// nobody touching it the glass goes back to the resting face by itself
// (Jake, 2026-11-21: "after a time of no interaction it will return to home").
//
// Hardware-free on purpose: the app loop feeds it one pass at a time and the
// host tests run the whole minute in a loop (firmware/tests/test_epaper_auto_home.cpp).
// It adds no wakeup of its own -- the loop already runs every 250 ms while the
// device rests and rebuilds the model once a second, and the router calls this
// from that same tick.
//
// Interaction is any key edge, and anything in flight suspends the count as if
// it were an interaction: a recording, a live turn, a phone waiting to be
// allowed, an upload (or the phone taking a note over Bluetooth), and a
// question Hermes asked (an abandoned question would otherwise be dismissed by
// a timer nobody asked for).
#pragma once

#include <cstdint>

#include "hg/ui.hpp"

namespace hgp {

// The faces the device rests on. Auto-home has nothing to do when one of them
// is already on the glass.
bool auto_home_face(hg::Screen screen);

// One minute, the product's number.
constexpr uint32_t kAutoHomeSeconds = 60;

class AutoHome {
 public:
  static constexpr uint32_t kIdleMs = kAutoHomeSeconds * 1000;

  struct Inputs {
    bool at_home = true;  // the resting face is on the glass already
    bool dozing = false;  // the panel is off: nothing to return from
    bool busy = false;    // a recording, a turn, an ask or an upload
  };

  // The clock starts here: a key edge, a screen change, or begin().
  void interacted(uint32_t now_ms) { last_ms_ = now_ms; }
  // Called once per resolved key edge (press or release, whatever it decided).
  bool idle_for(uint32_t now_ms) const { return now_ms - last_ms_ >= kIdleMs; }

  // One app-loop pass. True on the pass that should go home, and never twice
  // without another minute of quiet.
  bool tick(uint32_t now_ms, const Inputs& in);
  // The deadline moved for a reason the caller knows better (a key edge).
  uint32_t last_ms() const { return last_ms_; }

 private:
  uint32_t last_ms_ = 0;
};

}  // namespace hgp
