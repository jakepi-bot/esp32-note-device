// See audio_path.hpp: the transitions, and nothing else.
#include "notes_app/audio_path.hpp"

namespace hgn {

bool AudioPath::may_rest(const PathDemand& d) const {
  // A cable is a bench with a meter on it, and the port must stay up: the device
  // rests only on a cell, and only when nothing at all is going on. `capturing_`
  // is this machine's own record, so a caller that forgets to say so in the
  // demand cannot get the codec released under a capture. `asked` is the caller's
  // idle timeout: the machine decides what resting costs, not when a person has
  // stopped reading the screen.
  return d.asked && !d.on_cable && !d.capturing && !d.playing && !d.key_down && !d.busy && !capturing_ && !playing_;
}

PathActions AudioPath::rest() {
  if (state_ != PathState::Awake) return {};
  PathActions act;
  // Order is the audible part: the DAC is muted first, then the amplifier's
  // enable drops, then the microphone stops reading, and only then are the I2S
  // channels disabled. Anything after the release is a transient through a live
  // amplifier. All three are idempotent writes, so the transition repeats them
  // instead of keeping a shadow flag that can lie.
  act.mute_dac = true;
  if (caps_.pa_pin) act.pa_off = true;
  act.suspend_mic = true;
  if (!released_) {
    act.release_i2s = true;
    released_ = true;
  }
  if (!caps_.codec_shared && !codec_closed_) {
    // A codec the microphone does not share can be closed outright, which is
    // where the i2s lock goes with it. A shared one stays open: closing it resets
    // the chip under the microphone (the empty-note bug).
    act.power_down_codec = true;
    codec_closed_ = true;
  }
  if (caps_.panel_rail) act.rail_down = true;
  state_ = PathState::Idle;
  return act;
}

PathActions AudioPath::wake() {
  if (state_ != PathState::Idle) return {};
  PathActions act;
  if (codec_closed_) {
    act.power_up_codec = true;
    codec_closed_ = false;
  }
  if (released_) {
    act.restore_i2s = true;
    released_ = false;
  }
  act.resume_mic = true;
  // The mute is left alone: playback unmutes when it has audio, and a silent
  // revive is what keeps a press from clicking.
  if (caps_.panel_rail) act.rail_up = true;
  state_ = PathState::Awake;
  return act;
}

PathActions AudioPath::update(const PathDemand& d) {
  if (state_ == PathState::Asleep) return {};
  capturing_ = d.capturing;
  playing_ = d.playing || playing_;
  if (state_ == PathState::Awake) {
    if (!may_rest(d)) return {};
    return rest();
  }
  // Idle: only a reason to work leaves it. A cable alone is not one - the first
  // version woke on the cable's presence and an `epd idle on` never survived a
  // loop pass.
  if (d.capturing || d.playing || d.key_down || d.busy) return wake();
  return {};
}

PathActions AudioPath::press() {
  if (state_ != PathState::Idle) return {};
  return wake();
}

PathActions AudioPath::begin_capture() {
  capturing_ = true;
  // A capture needs live channels, in the same pass, before the first read.
  return wake();
}

PathActions AudioPath::doze_start() {
  if (state_ == PathState::Asleep) return {};
  const PathState before = state_;
  state_ = PathState::Asleep;
  // From Idle the codec is already shut and the rail is down: touching it here
  // was the pop heard once per sleep cycle. From Awake only a codec the
  // microphone does not share may be closed, and only if it is not already.
  if (before != PathState::Awake) return {};
  PathActions act;
  if (!caps_.codec_shared && !codec_closed_) {
    act.power_down_codec = true;
    codec_closed_ = true;
  }
  return act;
}

PathActions AudioPath::doze_end() {
  if (state_ != PathState::Asleep) return {};
  // Back to the state the sleep interrupted. An idle device stays shut; an awake
  // one gets its channels back, and whatever closed the codec reopens it.
  state_ = released_ ? PathState::Idle : PathState::Awake;
  if (state_ == PathState::Idle) return {};
  PathActions act;
  if (codec_closed_) {
    act.power_up_codec = true;
    codec_closed_ = false;
  }
  if (released_) {
    act.restore_i2s = true;
    released_ = false;
  }
  act.resume_mic = true;
  return act;
}

PathActions AudioPath::force_idle(bool idle) {
  if (idle) {
    // Whatever the demand says: this is a person (or a bench) asking.
    return rest();
  }
  return wake();
}

}  // namespace hgn
