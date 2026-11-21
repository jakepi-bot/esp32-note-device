// See live_state.hpp.
#include "modes/live_state.hpp"

namespace hgp {
namespace {
constexpr const char* kLostDetail = "Hermes connection lost";
}

void LiveState::transport(bool open, bool in_turn, UiPhase ui_phase, std::string why) {
  push_ = clear_ = false;
  if (open) {
    was_open_ = true;
    if (mine_ == UiPhase::Error) {
      clear_ = true;
      mine_ = UiPhase::Idle;
    }
    return;
  }
  // A close takes the line only when it cut something: a turn, or a
  // connection this device actually had. A device that has never connected
  // stays silent — the core's Offline screen and the talk hint say it.
  const bool claim = in_turn || was_open_;
  was_open_ = false;
  if (!claim) return;
  if (mine_ == UiPhase::Error) {
    if (ui_phase == UiPhase::Error) {
      clear_ = true;  // a fresh attempt re-publishes the line
    } else if (ui_phase != UiPhase::Idle) {
      return;  // the line changed hands: yield
    } else {
      mine_ = UiPhase::Idle;
    }
  } else if (mine_ == UiPhase::Idle && ui_phase != UiPhase::Idle) {
    return;  // another mode owns the line
  }
  push_ = true;
  push_phase_ = UiPhase::Error;
  detail_ = why.empty() ? std::string(kLostDetail) : "Hermes: " + why;
  mine_ = UiPhase::Error;
}

void LiveState::update(hg::Screen screen, UiPhase ui_phase) {
  push_ = clear_ = false;
  if (ui_phase == UiPhase::Error) return;  // an error leaves only through transport(true)
  UiPhase want = UiPhase::Idle;
  if (screen == hg::Screen::Listening) want = UiPhase::Recording;
  else if (screen == hg::Screen::Thinking) want = UiPhase::Uploading;
  if (want == UiPhase::Idle) {
    if (mine_ == UiPhase::Idle) return;
    if (ui_phase != mine_) {
      mine_ = UiPhase::Idle;  // the line changed hands: leave it
      return;
    }
    push_ = true;
    push_phase_ = UiPhase::Idle;
    detail_.clear();
    mine_ = UiPhase::Idle;
    return;
  }
  if (ui_phase != want) {
    push_ = true;
    push_phase_ = want;
    detail_.clear();
  }
  mine_ = want;
}

}  // namespace hgp
