// The e-paper port's live-mode decisions (LiveState) and BOOT's long press
// (TalkHold) are hardware-free by design, so they run here instead of on a
// board: what claims the notice line, what leaves it alone, and when a hold
// taps CANCEL.
#include "check.hpp"

#include "modes/live_mode.hpp"
#include "talk_hold.hpp"

using hgp::LiveState;
using hgp::TalkHold;
using hgp::UiPhase;
using hg::Screen;

namespace {
constexpr const char* kWhy = "disconnected";
}

TEST("the turn's screens drive the notice-line phase, and the line clears itself") {
  LiveState s;
  s.transport(true, false, UiPhase::Idle, "");
  CHECK(!s.should_push());
  s.update(Screen::Listening, UiPhase::Idle);
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Recording);
  CHECK_EQ(s.mine(), UiPhase::Recording);
  // The same screen keeps its phase without pushing every pass.
  s.update(Screen::Listening, UiPhase::Recording);
  CHECK(!s.should_push());
  s.update(Screen::Thinking, UiPhase::Recording);
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Uploading);
  // The reply plays: a line this mode set goes with it.
  s.update(Screen::Responding, UiPhase::Uploading);
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Idle);
  CHECK_EQ(s.mine(), UiPhase::Idle);
  // With nothing of its own on screen it pushes nothing at all.
  s.update(Screen::Ready, UiPhase::Idle);
  CHECK(!s.should_push());
}

TEST("a connection lost mid-turn claims the line with the close reason") {
  LiveState s;
  s.transport(true, false, UiPhase::Idle, "");
  s.update(Screen::Listening, UiPhase::Idle);
  CHECK_EQ(s.mine(), UiPhase::Recording);
  s.transport(false, true, UiPhase::Recording, kWhy);
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Error);
  CHECK_EQ(s.detail(), std::string("Hermes: ") + kWhy);
  CHECK_EQ(s.mine(), UiPhase::Error);
  // While the error shows, the turn's screens push nothing (UiState: an error
  // leaves only through a reconnect).
  s.update(Screen::Listening, UiPhase::Error);
  CHECK(!s.should_push());
  // A failed retry clears and re-publishes the line instead of letting it
  // expire unnoticed.
  s.transport(false, true, UiPhase::Error, kWhy);
  CHECK(s.should_clear_error());
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Error);
  // The link comes back: the error clears and the turn's phase returns.
  s.transport(true, true, UiPhase::Error, "");
  CHECK(s.should_clear_error());
  CHECK(!s.should_push());
  s.update(Screen::Listening, UiPhase::Idle);
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Recording);
}

TEST("a loss while idle claims the line once; retries after that stay silent") {
  LiveState s;
  s.transport(true, false, UiPhase::Idle, "");
  s.transport(false, false, UiPhase::Idle, kWhy);
  CHECK(s.should_push());
  CHECK_EQ(s.phase(), UiPhase::Error);
  // The turn (if any) is over: a failed retry claims nothing, so the line
  // stays free for the offline-notes mode while the device sits disconnected.
  s.transport(false, false, UiPhase::Error, kWhy);
  CHECK(!s.should_push());
  CHECK(!s.should_clear_error());
}

TEST("a device that never connected claims nothing") {
  LiveState s;
  s.transport(false, false, UiPhase::Idle, "client init failed");
  CHECK(!s.should_push());
  s.update(Screen::Offline, UiPhase::Idle);
  CHECK(!s.should_push());
}

TEST("a phase another mode set is left alone") {
  LiveState s;
  s.transport(true, false, UiPhase::Idle, "");
  // The notes mode owns the line when a connection drops under it.
  s.transport(false, false, UiPhase::Recording, kWhy);
  CHECK(!s.should_push());
  // And a live turn that ends under it does not clear the line either.
  s.update(Screen::Ready, UiPhase::Recording);
  CHECK(!s.should_push());
  CHECK_EQ(s.mine(), UiPhase::Idle);
}

TEST("BOOT held past the threshold taps CANCEL, only for a press that found something to cancel") {
  CHECK(!hgp::hold_cancel_armed(false, Screen::Ready));  // a fresh press is talk
  CHECK(!hgp::hold_cancel_armed(false, Screen::Clock));
  CHECK(hgp::hold_cancel_armed(true, Screen::Ready));    // a turn in flight
  CHECK(hgp::hold_cancel_armed(false, Screen::Widget));  // an overlay took the screen
  CHECK(hgp::hold_cancel_armed(false, Screen::Prompt));

  TalkHold hold;
  hold.begin(2000);
  hold.press(1000, false);  // a press that armed nothing never fires
  CHECK(!hold.fired(4000));
  hold.release();

  hold.press(5000, true);
  CHECK(!hold.fired(6999));
  CHECK(hold.fired(7000));  // once at the threshold
  CHECK(!hold.fired(8000));  // and never twice for the same hold
  hold.release();
  hold.press(9000, true);  // re-armed for the next hold
  CHECK(hold.fired(11000));

  TalkHold off;
  off.begin(0);  // every other board: the long press does not exist
  off.press(0, true);
  CHECK(!off.fired(100000));
}
