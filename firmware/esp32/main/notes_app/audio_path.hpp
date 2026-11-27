// The audio path's one owner: what the codec, the microphones, the amplifier and
// the panel rail are doing, and the one function that moves between the three
// states a resting device has.
//
// # Why this exists
//
// The same five flags used to be written by four files - `g_ui.idle()`,
// `g_runtime.set_idle()`, `CodecAudio::idle_released()`, `CodecMic::suspended_`,
// `CodecSpeaker::powered_`/`can_power_down_` - plus a pair of light-sleep
// callbacks that knew nothing about them. Three regressions in one day came out
// of that arrangement (2026-11-26/27):
//
//   * a capture started from idle stored nothing, because one path closed the
//     shared codec and the capture opened it too late;
//   * a pop per light-sleep cycle, because the callbacks reopened a codec the
//     idle path had just muted;
//   * a click once a second at rest, because a task polled a released codec.
//
// All three are "somebody changed the audio path from a place that did not own
// it". So the flags live here, the transitions are computed here, and the callers
// apply what they are handed. This file has no ESP-IDF and no FreeRTOS in it: the
// host test drives the same transitions the device does.
#pragma once

#include <cstdint>

namespace hgn {

// Where the audio path is. Three states, and nothing else may be inferred from
// the hardware read back at the call site.
enum class PathState : uint8_t {
  Awake,   // codec open, I2S running: a capture may start this instant
  Idle,    // the panel is dark: DAC muted, mic suspended, I2S released
  Asleep,  // light sleep, entered from Idle or Awake: touch nothing until it ends
};

// What the device is doing at this moment. A demand, not state: a capture that
// begins from Idle is itself the transition out of it.
struct PathDemand {
  bool capturing = false;  // a recording is running
  bool playing = false;    // audio is queued, or the codec still holds a tail
  bool key_down = false;   // a finger is on a key: the release must be sampled
  bool busy = false;       // the runtime has work that needs the codec
  bool on_cable = false;   // a cable is somebody's bench: never rest by itself
  bool asked = false;      // something asked the device to rest (the idle timeout)
};

// What the caller must do to the hardware. Every field is an action on a port;
// none of them is a state the caller keeps itself.
struct PathActions {
  bool mute_dac = false;      // the DAC goes quiet (the speaker has no signal)
  bool pa_off = false;        // the amplifier's enable is driven low
  bool suspend_mic = false;   // the microphone task waits instead of reading
  bool release_i2s = false;   // both I2S channels are disabled: light sleep can engage
  bool rail_down = false;     // the panel rail drops and every render is refused
  bool power_down_codec = false;  // only when the microphone does not share the codec
  bool restore_i2s = false;   // both I2S channels are enabled again
  bool resume_mic = false;    // the microphone task reads again
  bool rail_up = false;       // the panel rail comes back
  bool power_up_codec = false;    // only when the codec was closed

  bool any() const {
    return mute_dac || pa_off || suspend_mic || release_i2s || rail_down || power_down_codec || restore_i2s ||
           resume_mic || rail_up || power_up_codec;
  }
};

// What this board's audio path can do. The one thing that changes the shape of a
// transition: a codec the microphone shares must never be closed, because closing
// it resets the chip under a capture.
struct PathCaps {
  bool codec_shared = true;  // the ES8311 carries the speaker and the microphone
  bool pa_pin = true;        // an amplifier enable exists (GPIO 46 on this board)
  bool panel_rail = true;    // the panel's rail is under firmware control
};

class AudioPath {
 public:
  AudioPath() = default;
  explicit AudioPath(const PathCaps& caps) : caps_(caps) {}

  PathState state() const { return state_; }
  bool idle() const { return state_ == PathState::Idle; }
  bool awake() const { return state_ == PathState::Awake; }
  bool asleep() const { return state_ == PathState::Asleep; }
  // The I2S channels are disabled. Reading them while they are is the bug that
  // stored an empty note, so the caller asks rather than keeping its own flag.
  bool released() const { return released_; }

  // The loop's one call. From the demand, decide whether this pass may rest:
  // Idle -> (needs both a still device and no cable), Awake stays Awake while
  // anything is going on. A pass in Asleep changes nothing.
  PathActions update(const PathDemand& demand);

  // A press arrived. Restores the path before anything can read a microphone:
  // the capture that follows starts on live channels.
  PathActions press();

  // A capture is starting (or has started). Same restore, and from here on
  // update() may not release the codec under it.
  PathActions begin_capture();
  void end_capture() { capturing_ = false; }
  void set_playing(bool playing) { playing_ = playing; }

  // The light-sleep callbacks are part of this machine, not a second opinion
  // beside it: entering sleep from Idle has nothing to close, and doing it
  // anyway was one pop per cycle.
  PathActions doze_start();
  PathActions doze_end();

  // `epd idle on` / `epd idle off`: the bench's way into the resting state on a
  // cable. It must be the same transition a resting device takes, or the bench
  // measures a firmware nobody runs.
  PathActions force_idle(bool idle);

 private:
  PathActions rest();
  PathActions wake();
  bool may_rest(const PathDemand& d) const;

  PathCaps caps_{};
  PathState state_ = PathState::Awake;
  bool released_ = false;
  bool capturing_ = false;
  bool playing_ = false;
  bool codec_closed_ = false;
};

}  // namespace hgn
