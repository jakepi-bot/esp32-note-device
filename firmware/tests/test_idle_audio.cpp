// The audio path's state machine (notes_app/audio_path.*), driven the way the app
// drives it: awake -> idle -> press -> record -> stop -> idle.
//
// Three regressions in one day came out of these transitions being implicit - an
// empty note (the codec closed under a capture), a pop per light-sleep cycle (the
// callbacks reopened a resting codec) and a click once a second (a task polling a
// released codec). Each has a case here, against the same code the device runs.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "check.hpp"
#include "notes/notes_capture.hpp"
#include "notes/notes_store.hpp"
#include "notes_app/audio_path.hpp"

using hgn::AudioPath;
using hgn::PathActions;
using hgn::PathDemand;
using hgn::PathState;
using hgp::notes::Capture;
using hgp::notes::Meta;
using hgp::notes::NoteState;
using hgp::notes::Store;

namespace {

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/hg-audio-path-XXXXXX";
    const char* made = mkdtemp(tmpl);
    path = made ? made : "/tmp/hg-audio-path-fallback";
  }
  ~TempDir() {
    const std::string cmd = "rm -rf '" + path + "'";
    if (std::system(cmd.c_str()) != 0) {
      // best-effort, like the other card-tree cases
    }
  }
};

PathDemand still() {
  PathDemand d;
  d.asked = true;  // the idle timeout has passed: a resting device
  return d;
}

}  // namespace

TEST("idle: the resting transition mutes, suspends and releases, and drops the rail") {
  AudioPath path;
  const PathActions act = path.update(still());
  CHECK(path.idle());
  CHECK_EQ(static_cast<int>(path.state()), static_cast<int>(PathState::Idle));
  CHECK(act.mute_dac);      // the DAC first: nothing after this is audible
  CHECK(act.pa_off);        // then the amplifier's enable
  CHECK(act.suspend_mic);
  CHECK(act.release_i2s);
  CHECK(act.rail_down);
  // A codec the microphone shares is never closed - closing it resets the chip
  // under a capture, which is what stored an empty note.
  CHECK(!act.power_down_codec);
  CHECK(path.released());
}

TEST("idle: work in flight, a finger, or a cable keeps the path awake") {
  PathDemand d;
  d.capturing = true;
  {
    AudioPath path;
    CHECK(!path.update(d).any());
    CHECK(path.awake());
  }
  d = still();
  d.playing = true;
  {
    AudioPath path;
    CHECK(!path.update(d).any());
    CHECK(path.awake());
  }
  d = still();
  d.key_down = true;
  {
    AudioPath path;
    CHECK(!path.update(d).any());
    CHECK(path.awake());
  }
  d = still();
  d.busy = true;
  {
    AudioPath path;
    CHECK(!path.update(d).any());
    CHECK(path.awake());
  }
  d = still();
  d.on_cable = true;
  {
    AudioPath path;
    CHECK(!path.update(d).any());
    CHECK(path.awake());
  }
  // And once nothing is going on, the same demand rests.
  {
    AudioPath path;
    CHECK(path.update(still()).any());
    CHECK(path.idle());
  }
}

TEST("idle: the light-sleep callbacks do nothing to a codec that is already shut") {
  AudioPath path;
  path.update(still());
  CHECK(path.idle());
  // The pop per sleep cycle: entering light sleep from Idle must touch nothing.
  const PathActions in = path.doze_start();
  CHECK(!in.any());
  CHECK(path.asleep());
  const PathActions out = path.doze_end();
  CHECK(!out.any());
  CHECK(path.idle());  // an idle device stays idle across the sleep
  CHECK(path.released());

  // Awake, with a codec the microphone shares: nothing to close either.
  AudioPath awake;
  const PathActions shut = awake.doze_start();
  CHECK(!shut.power_down_codec);
  CHECK(awake.asleep());
}

TEST("idle: only a codec the microphone does not share is closed on a doze") {
  hgn::PathCaps caps;
  caps.codec_shared = false;
  AudioPath path(caps);
  const PathActions in = path.doze_start();
  CHECK(in.power_down_codec);
  CHECK(path.asleep());
  const PathActions out = path.doze_end();
  CHECK(out.power_up_codec);
  CHECK(path.awake());
  CHECK(!path.released());
}

TEST("idle: a press restores the channels before anything can read them") {
  AudioPath path;
  path.update(still());
  CHECK(path.released());
  const PathActions act = path.press();
  CHECK(act.restore_i2s);
  CHECK(act.resume_mic);
  CHECK(!act.release_i2s);
  CHECK(!path.released());
  CHECK(path.awake());
  // The mute stays: playback unmutes when it has audio, so a press is silent.
  CHECK(!act.mute_dac);
}

TEST("idle: a capture from idle lands a note with samples") {
  TempDir dir;
  Store store(dir.path);
  CHECK(store.ensure());

  AudioPath path;
  path.update(still());  // the device rests
  CHECK(path.idle());

  // The press, then the capture, through the machine - the order the loop uses.
  path.press();
  const PathActions begin = path.begin_capture();
  CHECK(!begin.release_i2s);
  CHECK(!path.released());

  Meta meta;
  meta.note_id = hgp::notes::make_note_id("hg-0123456789abcdef", 1795140312, 11);
  meta.device_id = "hg-0123456789abcdef";
  meta.device_name = "EPaper";
  meta.firmware = "0.7.1";
  meta.created_ms = 1795140312000;
  meta.recorded_at = "2026-11-20T02:05:12Z";
  meta.idempotency_key = meta.note_id;

  Capture capture;
  std::string error;
  CHECK(capture.start(store, meta.note_id, 300, error));
  // A microphone that can only deliver on live channels: exactly the empty-note
  // bug, which no amount of feeding hides if the path is still released.
  std::vector<int16_t> block(2400, 700);
  PathDemand recording = still();
  recording.capturing = true;
  for (int i = 0; i < 5; ++i) {
    if (path.released()) continue;
    capture.feed(block.data(), block.size());
    // The loop's own check, ten times a second: it must not release under us.
    CHECK(!path.update(recording).any());
  }
  CHECK_EQ(capture.bytes(), uint32_t(2400 * 2 * 5));
  CHECK(capture.finish(meta, error));
  CHECK_EQ(meta.bytes, 24000u);  // five blocks of 2400 samples, 16-bit mono

  const std::string wav = store.audio_path(NoteState::Pending, meta.note_id);
  struct stat st{};
  CHECK(stat(wav.c_str(), &st) == 0);
  CHECK_EQ(static_cast<uint32_t>(st.st_size), 44u + 24000u);

  // Stop, then rest again: the same transition the entry took.
  path.end_capture();
  const PathActions rest = path.update(still());
  CHECK(rest.mute_dac);
  CHECK(rest.suspend_mic);
  CHECK(rest.release_i2s);
  CHECK(rest.rail_down);
  CHECK(path.idle());
}

TEST("idle: `epd idle on` takes the same transition a resting device takes") {
  AudioPath automatic;
  const PathActions auto_act = automatic.update(still());

  AudioPath bench;
  const PathActions bench_act = bench.force_idle(true);

  CHECK(automatic.idle() && bench.idle());
  CHECK_EQ(auto_act.mute_dac, bench_act.mute_dac);
  CHECK_EQ(auto_act.suspend_mic, bench_act.suspend_mic);
  CHECK_EQ(auto_act.release_i2s, bench_act.release_i2s);
  CHECK_EQ(auto_act.rail_down, bench_act.rail_down);
  CHECK_EQ(auto_act.pa_off, bench_act.pa_off);

  // And leaving by either door is the same too.
  PathDemand recording;
  recording.capturing = true;
  const PathActions auto_out = automatic.update(recording);
  const PathActions bench_out = bench.force_idle(false);
  CHECK_EQ(auto_out.restore_i2s, bench_out.restore_i2s);
  CHECK_EQ(auto_out.resume_mic, bench_out.resume_mic);
  CHECK_EQ(auto_out.rail_up, bench_out.rail_up);
}
