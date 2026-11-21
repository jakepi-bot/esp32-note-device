// The notes device's key map (modes/home_keys.hpp) and the pairing ask's keys:
// PWR talks, BOOT records on a tap and cancels when held, and a press on its way
// to the power latch acts nothing. Hardware-free, so it runs here rather than on
// a desk.
#include "check.hpp"

#include <cstdint>

#include "modes/home_keys.hpp"

using namespace hgp;

namespace {

constexpr uint32_t kTap = 120;      // a firm tap
constexpr uint32_t kCancel = 2000;  // cancel_hold_ms
constexpr uint32_t kLatch = 2600;   // past the 2.5 s latch power-off

}  // namespace

TEST("a PWR tap talks and a press on its way to the latch does not") {
  HomeKeys k;
  k.begin({});
  CHECK(k.power_press(1000) == HomeAction::None);  // nothing happens on the press
  CHECK(k.power_release(1000 + kTap) == HomeAction::TalkToggle);
  // The same key held down is the hardware power-off: no microphone first.
  CHECK(k.power_press(5000) == HomeAction::None);
  CHECK(k.power_release(5000 + kLatch) == HomeAction::None);
  // A release with no press is not a press (a bounce, or a boot with the key down).
  CHECK(k.power_release(9000) == HomeAction::None);
  // And every short press talks, so the second one is how a turn ends: the caller
  // toggles (App::talk_toggle).
  CHECK(k.power_press(20000) == HomeAction::None);
  CHECK(k.power_release(20000 + kTap) == HomeAction::TalkToggle);
}

TEST("a BOOT tap toggles the note, and BOOT never talks however long it is held") {
  HomeKeys k;
  k.begin({});
  CHECK(k.key_press(1000) == HomeAction::None);
  CHECK(k.tick(1000 + kTap) == HomeAction::None);  // still inside a tap
  CHECK(k.key_release(1000 + kTap) == HomeAction::NoteToggle);
  // The same tap again stops the note: the caller toggles.
  CHECK(k.key_press(2000) == HomeAction::None);
  CHECK(k.key_release(2000 + kTap) == HomeAction::NoteToggle);

  // Held: cancel fires once at the threshold, while the key is still down, and
  // the lift that follows is not a tap (or a second note would start on release).
  CHECK(k.key_press(5000) == HomeAction::None);
  CHECK(k.tick(5000 + kCancel - 1) == HomeAction::None);
  CHECK(k.tick(5000 + kCancel) == HomeAction::Cancel);
  CHECK(k.tick(5000 + kCancel + 500) == HomeAction::None);  // fires once
  CHECK(k.key_release(5000 + kCancel + 600) == HomeAction::None);
  // A second hold is armed again by its own press.
  CHECK(k.key_press(10000) == HomeAction::None);
  CHECK(k.tick(10000 + kCancel) == HomeAction::Cancel);
  CHECK(k.key_release(10000 + kCancel + 10) == HomeAction::None);
}

TEST("a press past the tap limit is neither a note nor a cancel") {
  HomeKeys k;
  k.begin({});
  // Past tap_max_ms but under the cancel hold: on its way to the latch, or a stuck
  // key. Nothing happens, on the way down or up.
  CHECK(k.key_press(9000) == HomeAction::None);
  CHECK(k.tick(9000 + 1800) == HomeAction::None);
  CHECK(k.key_release(9000 + 1800) == HomeAction::None);
}

TEST("the pairing ask answers on the keys: BOOT allows, PWR refuses, a hold does neither") {
  // The requirement-5 acknowledgement: someone at the device decides, so a phone
  // in radio range cannot bond itself to a merely advertising device.
  CHECK(pairing_ask_answer(HomeAction::NoteToggle) == AskAnswer::Allow);
  CHECK(pairing_ask_answer(HomeAction::TalkToggle) == AskAnswer::Deny);
  CHECK(pairing_ask_answer(HomeAction::Cancel) == AskAnswer::None);
  CHECK(pairing_ask_answer(HomeAction::None) == AskAnswer::None);

  // And the map itself produces those actions from the real presses.
  HomeKeys k;
  k.begin({});
  CHECK(k.key_press(1000) == HomeAction::None);
  CHECK(pairing_ask_answer(k.key_release(1000 + kTap)) == AskAnswer::Allow);
  CHECK(k.power_press(2000) == HomeAction::None);
  CHECK(pairing_ask_answer(k.power_release(2000 + kTap)) == AskAnswer::Deny);
  // Twenty seconds with no answer is the core's own timeout, which refuses
  // (App::kPhoneAskMs; firmware/tests/test_app.cpp "a phone asking to pair waits
  // for Allow..."); no key can answer it once it has passed.
}

TEST("a press the poller sees mid-hold cannot act twice inside one press") {
  HomeKeys k;
  k.begin({});
  CHECK(k.key_press(100) == HomeAction::None);
  CHECK(k.tick(100 + kCancel) == HomeAction::Cancel);
  // A second press while the first is still down (a bounce the poller missed, or
  // a finger that never lifted): the cancel is re-armed with its own press.
  CHECK(k.key_press(2000) == HomeAction::None);
  CHECK(k.tick(2010) == HomeAction::None);
  CHECK(k.tick(2000 + kCancel) == HomeAction::Cancel);
  CHECK(k.key_release(2000 + kCancel + 50) == HomeAction::None);
}
