# The idle, power and audio path

The notes device has three resting states and one function that moves between
them. This page is the map, written after three regressions in one day came out
of the transitions being implicit:

- a note recorded from idle stored nothing, because one path closed the shared
  codec and the capture opened it too late;
- a pop once per light-sleep cycle, because the sleep callbacks reopened a codec
  the idle path had just muted;
- a click once a second at rest, because two tasks waited on a released codec with
  a one-second timeout.

## The three states

| State | Codec | I2S | Microphone | Amplifier | Panel rail |
| --- | --- | --- | --- | --- | --- |
| `Awake` | open, DAC live | enabled | reading | per playback | up |
| `Idle` | open, DAC muted | disabled | suspended | enable low | down |
| `Asleep` | untouched | untouched | untouched | untouched | untouched |

`Asleep` is light sleep. It is a pass-through state: entering and leaving it
changes nothing on the audio path, because `Idle` has already done the work. That
is the whole point - the per-cycle pop was `on_doze_start` closing a codec that
was already shut.

The codec is never closed on this board. The ES8311 carries both the speaker and
the analog microphone, so closing the handle resets the chip under the
microphone: the capture then runs, stops, and is discarded with no samples. A
board whose microphone has its own codec (`PathCaps::codec_shared = false`) may
close it, and does.

## The transitions

One owner: `hgn::AudioPath` in `firmware/esp32/main/notes_app/audio_path.*`. The
caller builds a `PathDemand` (a capture is running, audio is playing, a key is
down, the runtime is busy, the cable is in, the idle timeout has passed) and gets
a `PathActions` back: mute the DAC, drop the amplifier's enable, suspend the
microphone, release the I2S channels, drop the panel rail - or the reverse.

| From | Event | To | Actions |
| --- | --- | --- | --- |
| `Awake` | nothing going on, `asked`, on a cell | `Idle` | mute, amp off, suspend mic, release I2S, rail down |
| `Idle` | a press | `Awake` | restore I2S, resume mic, rail up |
| `Idle` | a capture starts | `Awake` | the same, in the same pass, before the first chunk |
| `Idle` | light sleep | `Asleep` | none |
| `Awake` | light sleep | `Asleep` | close the codec, only if it is not shared |
| `Asleep` | wake | the state it left | what that state needs |

Two rules make the class of bug impossible rather than merely unlikely:

- **Nothing else may release the codec.** A capture holds the machine, and the
  machine will not release the I2S while it holds a capture - the host case
  `idle: a capture from idle lands a note with samples` walks
  awake → idle → press → record → stop → idle and fails if the note has no
  samples.
- **The rail follows the machine.** `g_ui.idle()` and `g_runtime.set_idle()` are
  set from the action set, not remembered separately by each caller.

## Listening at rest

The click was a current step on the supply of the codec and its amplifier,
amplified by an amplifier that was still enabled while the DAC was muted. Two
things feed it, and both are gone:

1. **The speaker task waited with a one-second timeout** while the codec was
   released - one CPU wake per second, for the life of the device. It waits with
   no timeout now, and `write()`, `abort()` and `begin()` notify it. The
   microphone task had the same bug (fixed first); the speaker's was the twin
   that survived.
2. **The amplifier's enable stayed high** through idle, because `esp_codec_dev`
   turns the `pa_pin` on when the codec opens and leaves it on. Resting drives it
   low (`CodecSpeaker::amp_off`), so even a wake cannot reach the speaker.

The `wakes` console verb reports what the firmware actually measured: light-sleep
exits split by cause (timer, GPIO), the speaker task's resting waits, and the time
spent asleep. A resting device should leave light sleep about once per idle wait
(10 s) and enter the speaker's resting wait zero times.

## Measuring it on the bench

A resting chip cannot answer its own console: the USB-Serial-JTAG port is not a
wake source, so the reading has to survive the reset that brings it back. Both
counters live in RTC memory for that reason, and `wakes reset` zeroes them.

```sh
# a bench build that sleeps on the cable too (the shipped one does not, on purpose)
cd firmware/esp32
PYTHONPATH= ~/.hermes/tools/platformio-venv/bin/pio run -e notes -t upload \
  --project-option "build_flags=-DHG_BENCH_LIGHT_SLEEP"
```

Then `tools/idle_wake_probe.py` does the whole run: zero the counters, `epd idle
on`, wait for the console to go quiet (that is the chip sleeping), rest, reset the
chip over USB, and read the counters back. It fails above `--gate` wakes per
second.

Two bench traps, both met while writing this:

- A bench build that sleeps on the cable can wedge the USB link: the port stops
  accepting control transfers and `esptool` answers `EPROTO`. Toggling the port's
  `authorized` file catches one of the chip's awake windows and brings it back.
- The console's own host-wait task polls every 500 ms until a USB host appears,
  so a rest measured with the cable in is never quieter than that. Do not read a
  sleep duty off a bench run; read the wake counts, which is what the probe does.
