// The e-paper notes device's resting face: modes/status_screen.cpp draws it,
// this file decides when it owns the panel and feeds it the device's facts.
//
// Ownership is a hand-off, not a fight. The core UI paints the panel for
// everything that needs a key or a turn (boot, connecting, pairing, listening,
// thinking, responding, settings, prompts, errors, the clock face); while the
// device rests -- ready, offline, or on one of the battery faces -- this screen
// takes the whole 200x200. App::rest_screen is the hand-off (core/src/app.cpp),
// and the change check inside StatusScreen keeps an idle panel from being
// refreshed at all.
//
// It also forces a full refresh on each hand-over: a partial update from the
// core UI's white-on-black screen to this one leaves the previous image
// ghosted, and the panel's own "full every 20 partials" rule is too slow to
// hide it.
//
// There is no touch glass on this product (2026-11-21), so this screen has
// no targets: the two keys are the whole input surface and every hint says which
// key does what. The one screen it takes from the core's Setup is the pairing
// ask, because the answer has to be readable on paper and the buttons answer it.
#pragma once

#include <cstdint>

#include "hg/hal.hpp"
#include "modes/notes_mode.hpp"
#include "modes/status_screen.hpp"

namespace hg {
class App;
class UiModel;
}  // namespace hg

namespace hgp {

class EpdDisplay;

class NotesScreen {
 public:
  // The board's services. `system` supplies the clock; the battery level and the
  // supply come from App, which already polls them and applies the board's
  // battery rules (gauge, or the voltage estimate) once.
  void begin(hg::App& app, NotesMode& notes, hg::System* system, EpdDisplay& display);

  // App::rest_screen: draws the status screen when the device is resting and
  // returns true when this screen holds the panel afterwards. `force` says the
  // core UI painted the frame before this one, so the glass is not this
  // screen's image any more.
  bool render(const hg::UiModel& model, bool force);

 private:
  StatusInputs gather();
  bool draw(const StatusView& view, bool repaint);

  hg::App* app_ = nullptr;
  NotesMode* notes_ = nullptr;
  hg::System* system_ = nullptr;
  EpdDisplay* display_ = nullptr;

  StatusScreen screen_;
  bool owned_ = false;  // this screen holds the panel
};

}  // namespace hgp
