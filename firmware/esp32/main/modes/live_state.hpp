// LiveState: what the live mode's notice line shows, decided from the
// transport and the app's screen. Hardware-free so the host tests drive it
// (firmware/tests/test_epaper_live_mode.cpp); LiveModeImpl in live_mode.cpp
// is the ESP32 wiring over App and UiState.
#pragma once

#include <string>

#include "hg/ui.hpp"
#include "ui_state.hpp"

namespace hgp {

// It claims the notice line only while something of its own is on it: an idle
// screen stays clear for the offline-notes mode, and a phase another mode set
// is left alone. Two rules the tests pin:
//   - a close claims the line only when it cut a turn, or a connection this
//     device actually had; a device that never connected stays silent (the
//     core's Offline screen and the talk hint say it);
//   - an Error on the line leaves only through a reconnect, so a failed retry
//     re-publishes it instead of letting it expire unnoticed.
class LiveState {
 public:
  // The transport opened or closed. in_turn is the app's state at that
  // moment, ui_phase what is on the notice line, why the close reason.
  void transport(bool open, bool in_turn, UiPhase ui_phase, std::string why);
  // One app-loop pass: the app's screen and the phase on the notice line.
  void update(hg::Screen screen, UiPhase ui_phase);

  // What to apply to UiState right after the call; both are one-shot.
  bool should_clear_error() const { return clear_; }
  bool should_push() const { return push_; }
  UiPhase phase() const { return push_phase_; }
  const std::string& detail() const { return detail_; }
  // The phase this mode last pushed; Idle when none of its own is on screen.
  UiPhase mine() const { return mine_; }

 private:
  UiPhase mine_ = UiPhase::Idle;
  UiPhase push_phase_ = UiPhase::Idle;
  std::string detail_;
  bool push_ = false, clear_ = false, was_open_ = false;
};

}  // namespace hgp
