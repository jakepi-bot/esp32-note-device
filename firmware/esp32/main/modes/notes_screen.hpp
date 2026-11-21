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
// The touch targets are the strip along the bottom: TALK/STOP, which starts and
// stops a live turn (App::touch_talk_toggle), and -- while a phone asks to pair --
// the NO/YES boxes that answer it (App::answer_phone_pairing). Both are drawn
// only on a unit whose glass answered on the I2C bus (the product's Touch SKU);
// the plain SKU draws no strip, and the keys are the way in.
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
  // The board's services. `system` supplies the clock; the battery level and
  // the supply come from App, which already polls them and applies the board's
  // battery rules (gauge, or the voltage estimate) once. `touch` says the glass
  // answered on the I2C bus, so the strip is drawn and a tap is read.
  void begin(hg::App& app, NotesMode& notes, hg::System* system, EpdDisplay& display, bool touch);

  // App::rest_screen: draws the status screen when the device is resting and
  // returns true when this screen holds the panel afterwards. `force` says the
  // core UI painted the frame before this one, so the glass is not this
  // screen's image any more.
  bool render(const hg::UiModel& model, bool force);

  // A touch sample from the input task, delivered on the app task. True when this
  // screen consumed it: while it owns the glass, the strip is the only target and
  // a tap anywhere else is swallowed rather than opening the microphone.
  bool touch(bool touching, int x, int y, uint32_t now_ms);

 private:
  StatusInputs gather();
  bool draw(const StatusView& view, bool repaint);

  hg::App* app_ = nullptr;
  NotesMode* notes_ = nullptr;
  hg::System* system_ = nullptr;
  EpdDisplay* display_ = nullptr;
  bool touch_ = false;  // the glass has a controller

  StatusScreen screen_;
  bool owned_ = false;  // this screen holds the panel
  // The face on the glass, so a tap is hit-tested against what the user sees.
  StatusView last_;

  // The touch in progress: a tap is judged on release, inside the box it started
  // in and soon after it started, so a hand resting on the device presses nothing.
  bool touching_ = false;
  int down_x_ = 0, down_y_ = 0;
  uint32_t down_ms_ = 0;
};

}  // namespace hgp
