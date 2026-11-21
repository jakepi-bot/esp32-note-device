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
  CHECK(k.tick(5000 + kCancel) == HomeAction::Hold);
  CHECK(k.tick(5000 + kCancel + 500) == HomeAction::None);  // fires once
  CHECK(k.key_release(5000 + kCancel + 600) == HomeAction::None);
  // A second hold is armed again by its own press.
  CHECK(k.key_press(10000) == HomeAction::None);
  CHECK(k.tick(10000 + kCancel) == HomeAction::Hold);
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
  CHECK(pairing_ask_answer(HomeAction::Hold) == AskAnswer::None);
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
  CHECK(k.tick(100 + kCancel) == HomeAction::Hold);
  // A second press while the first is still down (a bounce the poller missed, or
  // a finger that never lifted): the cancel is re-armed with its own press.
  CHECK(k.key_press(2000) == HomeAction::None);
  CHECK(k.tick(2010) == HomeAction::None);
  CHECK(k.tick(2000 + kCancel) == HomeAction::Hold);
  CHECK(k.key_release(2000 + kCancel + 50) == HomeAction::None);
}

namespace {

KeyContext home_context() { return KeyContext{}; }
KeyContext menu_context() {
  KeyContext c;
  c.menu_open = true;
  c.turn_or_overlay = true;  // the menu covers whatever was there
  return c;
}
KeyContext recording_context() {
  KeyContext c;
  c.note_recording = true;
  return c;
}
KeyContext turn_context() {
  KeyContext c;
  c.turn_or_overlay = true;
  return c;
}
KeyContext ask_context() {
  KeyContext c;
  c.phone_ask = true;
  c.turn_or_overlay = true;
  return c;
}

}  // namespace

TEST("held BOOT on the home screen opens the menu, which is the only way in") {
  // PWR cannot be half of a chord: holding it is the latch's power-off, so the
  // menu is the held BOOT and nothing else. On a desk this was the bug: the
  // documented TALK+CANCEL chord could never fire.
  CHECK(route_key(HomeAction::Hold, home_context()) == KeyRoute::MenuOpen);
  // A tap at home is still a note, and a PWR tap is still a turn.
  CHECK(route_key(HomeAction::NoteToggle, home_context()) == KeyRoute::NoteToggle);
  CHECK(route_key(HomeAction::TalkToggle, home_context()) == KeyRoute::TalkToggle);
}

TEST("in the menu a BOOT tap is next and a PWR tap is change") {
  CHECK(route_key(HomeAction::NoteToggle, menu_context()) == KeyRoute::MenuTapNext);
  CHECK(route_key(HomeAction::TalkToggle, menu_context()) == KeyRoute::MenuTapChange);
  // A tap in the menu never starts a note or a turn.
  CHECK(route_key(HomeAction::NoteToggle, menu_context()) != KeyRoute::NoteToggle);
  CHECK(route_key(HomeAction::TalkToggle, menu_context()) != KeyRoute::TalkToggle);
  // The same hold that opened it closes it.
  CHECK(route_key(HomeAction::Hold, menu_context()) == KeyRoute::MenuClose);
}

TEST("a held BOOT drops a recording before it does anything else") {
  // The recording is the most valuable thing on the device, so it wins even if
  // a turn or the menu is also up.
  KeyContext c = recording_context();
  c.turn_or_overlay = true;
  CHECK(route_key(HomeAction::Hold, c) == KeyRoute::CancelNote);
  CHECK(route_key(HomeAction::NoteToggle, c) == KeyRoute::NoteToggle);  // a tap stops and keeps it
}

TEST("a held BOOT with a turn or a card up is the old cancel, not the menu") {
  CHECK(route_key(HomeAction::Hold, turn_context()) == KeyRoute::CancelTurn);
}

TEST("the pairing ask outranks every other meaning of a key") {
  CHECK(route_key(HomeAction::NoteToggle, ask_context()) == KeyRoute::AskAllow);
  CHECK(route_key(HomeAction::TalkToggle, ask_context()) == KeyRoute::AskDeny);
  CHECK(route_key(HomeAction::Hold, ask_context()) == KeyRoute::None);  // a hold never answers
  CHECK(route_key(HomeAction::None, home_context()) == KeyRoute::None);
}
