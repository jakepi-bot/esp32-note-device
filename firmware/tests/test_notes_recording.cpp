// The recording face (t_704171a7): the entry frame, the counter's cadence, and
// the rule that the glass follows the runtime's capture instead of its own
// optimistic flag.
#include <cstdio>

#include "check.hpp"

#include "notes_app/ui_app.hpp"

using namespace hgp::notesui;

namespace {

Snapshot recording_snapshot() {
  Snapshot snap;
  snap.card = true;
  snap.note_max_s = 3600;
  return snap;
}

}  // namespace

TEST("recording: the entry frame is 0:00 and the counter moves every 30 s") {
  ScreenApp app;
  app.begin(recording_snapshot());

  const Outcome start = app.press(Key::BootTap, 1000);
  CHECK(start.effect == Effect::BeginRecording);
  CHECK(start.draw);
  CHECK(start.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Recording);
  // What the entry frame says: the counter starts at zero, on the glass, before
  // any tick runs.
  CHECK(app.frame().recording.elapsed_s == 0);

  // One second in, nine, twenty-nine: no frame. The glass keeps 0:00.
  for (uint32_t elapsed = 1; elapsed < 30; ++elapsed) {
    const Outcome out = app.tick(1000 + elapsed * 1000);
    CHECK(!out.draw);
    CHECK(app.frame().recording.elapsed_s == 0);
  }

  // Thirty seconds: one partial, and the counter reads the half minute.
  const Outcome at30 = app.tick(1000 + 30 * 1000);
  CHECK(at30.draw);
  CHECK(at30.refresh == Refresh::Partial);
  CHECK(app.frame().recording.elapsed_s == 30);

  // Under the next half minute nothing moves again; at one minute it does.
  for (uint32_t elapsed = 31; elapsed < 60; ++elapsed) CHECK(!app.tick(1000 + elapsed * 1000).draw);
  const Outcome at60 = app.tick(1000 + 60 * 1000);
  CHECK(at60.draw);
  CHECK(app.frame().recording.elapsed_s == 60);

  // A tap still commits: the counter's cadence is not the capture's boundary.
  const Outcome stop = app.press(Key::BootTap, 1000 + 61 * 1000);
  CHECK(stop.effect == Effect::EndRecording);
  CHECK(app.screen() == Screen::Home);
}

TEST("capture: the glass follows the runtime, not its own flag") {
  ScreenApp app;
  app.begin(recording_snapshot());

  // A capture the console started (`notes record`): Home hands over, and the
  // counter's origin is the runtime's own second, not the moment Home noticed.
  const Outcome entered = app.follow_capture(true, 7, 10000);
  CHECK(entered.draw);
  CHECK(entered.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Recording);
  CHECK(app.frame().recording.elapsed_s == 7);
  CHECK(app.snapshot().recording);

  // A capture that ended with no key takes the glass off the recording face.
  // This is the counter that used to run on over a note already on the card.
  const Outcome left = app.follow_capture(false, 0, 40000);
  CHECK(left.draw);
  CHECK(left.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Home);
  CHECK(!app.snapshot().recording);

  // A screen the person is using keeps its place: a capture elsewhere does not
  // yank the glass, and the recorder runs either way.
  app.press(Key::BootHold, 0);
  CHECK(app.screen() == Screen::Menu);
  CHECK(!app.follow_capture(true, 0, 50000).draw);
  CHECK(app.screen() == Screen::Menu);

  // Agreeing with the runtime costs no frame.
  CHECK(!app.follow_capture(false, 0, 60000).draw);
}

TEST("capture: a declined recording never leaves RECORDING on the glass") {
  ScreenApp app;
  app.begin(recording_snapshot());

  const Outcome start = app.press(Key::BootTap, 1000);
  CHECK(start.effect == Effect::BeginRecording);
  CHECK(app.screen() == Screen::Recording);

  // The runtime said no (no microphone, no card, a note already in flight): the
  // screen goes back to Home and owes a full frame - the recording face is never
  // painted over a silence.
  const Outcome back = app.decline(Effect::BeginRecording);
  CHECK(back.draw);
  CHECK(back.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Home);
  CHECK(!app.snapshot().recording);
  CHECK(app.frame().recording.elapsed_s == 0);
}

TEST("capture: stopping a capture that never began owes no second frame") {
  ScreenApp app;
  app.begin(recording_snapshot());
  app.press(Key::BootTap, 1000);
  // The capture was already gone, so the runtime declines the stop too: the
  // screens' own move back to Home stands and its frame is the one drawn.
  const Outcome stop = app.press(Key::BootTap, 5000);
  CHECK(stop.effect == Effect::EndRecording);
  CHECK(app.screen() == Screen::Home);
  CHECK(!app.decline(Effect::EndRecording).draw);
  CHECK(app.screen() == Screen::Home);
}
