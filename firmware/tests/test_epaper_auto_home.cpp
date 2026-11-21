// The notes device's auto-home clock (modes/auto_home.hpp): after a quiet minute
// on any screen but home, the glass goes back to the resting face. The loop that
// drives it runs every 250 ms while the device rests, so that is the cadence this
// test uses; the minute itself is hardware-free and measured here.
#include "check.hpp"

#include <cstdint>

#include "modes/auto_home.hpp"

using namespace hgp;

namespace {

// The candidate the app loop actually performs: 250 ms while resting, and the
// model rebuilt once a second. Driving the clock at either rate must reach the
// same minute.
constexpr uint32_t kPassMs = 250;
constexpr uint32_t kSeconds = kAutoHomeSeconds;

// Runs the clock for `ms` and returns how many times it asked to go home.
int run(AutoHome& a, uint32_t& now, uint32_t ms, AutoHome::Inputs in) {
  int fires = 0;
  for (uint32_t i = 0; i < ms / kPassMs; ++i) {
    now += kPassMs;
    if (a.tick(now, in)) ++fires;
  }
  return fires;
}

}  // namespace

TEST("a minute on another screen asks for home once, and not a pass early") {
  AutoHome a;
  AutoHome::Inputs gone;  // on another screen, nothing in flight
  gone.at_home = false;
  CHECK_EQ(kSeconds, 60u);

  uint32_t now = 0;
  a.interacted(now);  // the press that left home
  CHECK_EQ(run(a, now, (kSeconds - 1) * 1000, gone), 0);
  // 59 s: still waiting.
  CHECK_EQ(run(a, now, 1000 - kPassMs, gone), 0);
  // The next pass is the minute: exactly one return home.
  CHECK_EQ(run(a, now, kPassMs, gone), 1);
  // And it does not ask again for another minute.
  CHECK_EQ(run(a, now, 59 * 1000, gone), 0);
  CHECK_EQ(run(a, now, 1000, gone), 1);
}

TEST("home, dozing and anything in flight suspend the count") {
  AutoHome a;
  uint32_t now = 0;
  a.interacted(now);

  AutoHome::Inputs home;  // at_home defaults true
  CHECK_EQ(run(a, now, 120 * 1000, home), 0);  // already home: nothing to return from

  AutoHome::Inputs dozing;
  dozing.at_home = false;
  dozing.dozing = true;
  CHECK_EQ(run(a, now, 120 * 1000, dozing), 0);  // the panel is off

  AutoHome::Inputs busy;  // recording, a turn, an ask or an upload
  busy.at_home = false;
  busy.busy = true;
  CHECK_EQ(run(a, now, 120 * 1000, busy), 0);

  // The busy spell ends: the minute counts from the pass it ended on, not from
  // before it (a 90 s recording must not return home one pass after it stops).
  AutoHome::Inputs gone;
  gone.at_home = false;
  CHECK_EQ(run(a, now, 59 * 1000, gone), 0);
  CHECK_EQ(run(a, now, 1000, gone), 1);
}

TEST("a key press restarts the minute") {
  AutoHome a;
  AutoHome::Inputs gone;
  gone.at_home = false;
  uint32_t now = 0;
  a.interacted(now);
  CHECK_EQ(run(a, now, 50 * 1000, gone), 0);
  // The user presses something: the debounced edge calls interacted().
  a.interacted(now);
  CHECK(!a.idle_for(now + 1000));
  CHECK_EQ(run(a, now, 59 * 1000, gone), 0);
  CHECK_EQ(run(a, now, 1000, gone), 1);
}

TEST("the resting faces are home; everything else is not") {
  CHECK(auto_home_face(hg::Screen::Ready));
  CHECK(auto_home_face(hg::Screen::Offline));
  CHECK(auto_home_face(hg::Screen::LowBattery));
  CHECK(auto_home_face(hg::Screen::Charging));
  CHECK(auto_home_face(hg::Screen::Charged));
  // A pairing code is waiting for a phone, a menu is open, a turn is running: each
  // is a screen the user (or Hermes) put there, and it gives way after the minute.
  CHECK(!auto_home_face(hg::Screen::Pairing));
  CHECK(!auto_home_face(hg::Screen::Settings));
  CHECK(!auto_home_face(hg::Screen::Prompt));
  CHECK(!auto_home_face(hg::Screen::Listening));
  CHECK(!auto_home_face(hg::Screen::Responding));
  CHECK(!auto_home_face(hg::Screen::Boot));
}
