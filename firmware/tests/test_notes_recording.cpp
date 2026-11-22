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

TEST("queue: a tap during an upload draws the wait, then counts when it starts") {
  ScreenApp app;
  app.begin(recording_snapshot());

  const Outcome tap = app.press(Key::BootTap, 1000);
  CHECK(tap.effect == Effect::BeginRecording);
  CHECK(app.screen() == Screen::Recording);

  // The runtime answered, and its answer is "kept, not started": the glass says
  // so in the place the counter would be.
  const Outcome queued = app.follow_capture(false, 0, 3000, true);
  CHECK(queued.draw);
  CHECK(queued.refresh == Refresh::Full);
  CHECK(app.screen() == Screen::Recording);
  CHECK(app.frame().recording.queued);
  CHECK(app.frame().recording.elapsed_s == 0);

  // Nothing counts while the tap waits, and the cap cannot arrive before the
  // capture does.
  for (uint32_t elapsed = 1; elapsed <= 40; ++elapsed) CHECK(!app.tick(3000 + elapsed * 1000).draw);
  CHECK(app.frame().recording.queued);
  CHECK(!app.follow_capture(false, 0, 60000, true).draw);  // already on the queued face

  // The upload ended and the capture began: the face stops saying QUEUED and the
  // counter takes the runtime's own second as its origin.
  const Outcome started = app.follow_capture(true, 2, 62000, false);
  CHECK(started.draw);
  CHECK(started.refresh == Refresh::Full);
  CHECK(!app.frame().recording.queued);
  CHECK(app.frame().recording.elapsed_s == 2);
  const Outcome at32 = app.tick(62000 + 30 * 1000);
  CHECK(at32.draw);
  CHECK(app.frame().recording.elapsed_s == 32);
}

TEST("queue: a tap on the queued face cancels the wait") {
  ScreenApp app;
  app.begin(recording_snapshot());
  app.press(Key::BootTap, 1000);
  app.follow_capture(false, 0, 3000, true);
  CHECK(app.frame().recording.queued);

  // The face says RECORDING, so its own key ends it: the runtime drops the arm
  // and the glass goes Home, with no note and no second frame owed.
  const Outcome cancel = app.press(Key::BootTap, 5000);
  CHECK(cancel.effect == Effect::EndRecording);
  CHECK(app.screen() == Screen::Home);
  const Outcome followed = app.follow_capture(false, 0, 6000, false);
  CHECK(!followed.draw);
  CHECK(app.screen() == Screen::Home);
}

TEST("queue: a stale counter origin cannot commit a capture the card says is empty") {
  ScreenApp app;
  app.begin(recording_snapshot());  // cap 3600 s
  app.press(Key::BootTap, 1000);
  CHECK(app.screen() == Screen::Recording);

  // The runtime says nothing has been written yet, but the screens' origin is old
  // enough that the local counter is past the cap. A cap commits a note, so the
  // glass must not be the one to decide the file is that long.
  const Outcome late = app.tick(1000 + (3600 + 400) * 1000);
  CHECK(late.effect != Effect::EndRecording);
  CHECK(app.screen() == Screen::Recording);
}

TEST("recording: the face carries the capture's own cap, and stops where the file stops") {
  // The setting says 24 h; the card had room for 1.5 h when the capture started
  // (a console `notes record`, so the glass adopts the runtime's own second). The
  // face must say 1.5 h - the note can only ever be that long - and the screen
  // must stop the capture there, not at the setting.
  Snapshot snap = recording_snapshot();
  snap.note_max_s = 24u * 60u * 60u;
  ScreenApp app;
  app.begin(snap);
  app.set_capture_cap(5400, 252ull * 1024ull * 1024ull);
  const Outcome entered = app.follow_capture(true, 5399, 6000000);  // 100 min of uptime
  CHECK(entered.draw);
  CHECK(app.screen() == Screen::Recording);
  CHECK_EQ(app.frame().recording.cap_s, 5400u);
  CHECK_EQ(app.frame().recording.cap_bytes, 252ull * 1024ull * 1024ull);
  CHECK_EQ(app.snapshot().note_max_s, 5400u);

  // A cap answer of 0 means "the probe has nothing to say", not "the cap is 0":
  // the capture's own cap stands.
  app.set_capture_cap(0, 0);
  CHECK_EQ(app.frame().recording.cap_s, 5400u);

  // At the cap's second the note is committed - 23 h before the setting would
  // have stopped it - and the glass goes Home. The runtime's second agrees, which
  // is what the gate asks for: a screen must not commit a note the card says is
  // empty.
  const Outcome stop = app.tick(6000000 - 5399 * 1000 + 5400 * 1000);
  CHECK(stop.effect == Effect::EndRecording);
  CHECK(app.screen() == Screen::Home);

  // And under the cap nothing is committed, whatever the setting says.
  ScreenApp under;
  under.begin(snap);
  under.set_capture_cap(5400, 252ull * 1024ull * 1024ull);
  under.follow_capture(true, 3000, 6000000);
  const Outcome midway = under.tick(6000000 - 3000 * 1000 + 3000 * 1000);
  CHECK(midway.effect != Effect::EndRecording);
  CHECK(under.screen() == Screen::Recording);
}
