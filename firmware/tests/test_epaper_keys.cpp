// The notes device's key map (modes/home_keys.hpp): PWR goes home, the second
// key records a note on a tap and talks on a hold, and a press on its way to the
// power latch acts nothing. Hardware-free, so it runs here rather than on a desk.
#include "check.hpp"

#include <cstdint>

#include "modes/home_keys.hpp"

using namespace hgp;

namespace {

constexpr uint32_t kTap = 120;      // a firm tap
constexpr uint32_t kHold = 900;     // past talk_hold_ms
constexpr uint32_t kLatch = 2600;   // past the 2.5 s latch power-off

}  // namespace

TEST("a PWR tap goes home and a press on its way to the latch does not") {
  HomeKeys k;
  k.begin({});
  CHECK(k.power_press(1000) == HomeAction::None);  // nothing happens on the press
  CHECK(k.power_release(1000 + kTap) == HomeAction::Home);
  // The same key held down is the hardware power-off: no home screen flash first.
  CHECK(k.power_press(5000) == HomeAction::None);
  CHECK(k.power_release(5000 + kLatch) == HomeAction::None);
  // A release with no press is not a press (a bounce, or a boot with the key down).
  CHECK(k.power_release(9000) == HomeAction::None);
}

TEST("the second key's tap records a note, and a hold talks instead") {
  HomeKeys k;
  k.begin({});
  CHECK(k.key_press(1000) == HomeAction::None);
  CHECK(k.tick(1000 + kTap) == HomeAction::None);  // still inside a tap
  CHECK(k.key_release(1000 + kTap) == HomeAction::NoteToggle);

  // A hold: talk starts on the threshold, and the lift is the end of the turn --
  // never a note, or a long press would record one on release.
  CHECK(k.key_press(5000) == HomeAction::None);
  CHECK(k.tick(5000 + kHold) == HomeAction::TalkStart);
  CHECK(k.tick(5000 + kHold + 200) == HomeAction::None);  // fires once
  CHECK(k.key_release(5000 + kHold + 400) == HomeAction::TalkStop);
}

TEST("with a touch glass the hold talks nothing, and a slow press records nothing") {
  HomeKeys k;
  HomeKeys::Config cfg;
  cfg.hold_talks = false;
  k.begin(cfg);
  CHECK(k.key_press(1000) == HomeAction::None);
  CHECK(k.tick(1000 + kHold) == HomeAction::None);  // the strip talks, not this key
  CHECK(k.key_release(1000 + kHold) == HomeAction::NoteToggle);  // still a tap

  // A press past tap_max_ms is on its way to the latch (or a stuck key): it is
  // neither a note nor a turn.
  CHECK(k.key_press(9000) == HomeAction::None);
  CHECK(k.key_release(9000 + kLatch) == HomeAction::None);
}

TEST("a press the poller sees mid-hold cannot talk twice inside one press") {
  HomeKeys k;
  k.begin({});
  CHECK(k.key_press(100) == HomeAction::None);
  CHECK(k.tick(100 + kHold) == HomeAction::TalkStart);
  // A second press while the first is still down (a bounce the poller missed, or
  // a finger that never lifted): no action until the new press passes the hold.
  CHECK(k.key_press(2000) == HomeAction::None);
  CHECK(k.tick(2010) == HomeAction::None);
  CHECK(k.tick(2000 + kHold) == HomeAction::TalkStart);
  CHECK(k.key_release(2000 + kHold + 50) == HomeAction::TalkStop);
}
