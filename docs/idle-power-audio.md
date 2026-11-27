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

### What the device actually measures, 2026-11-27

Bench build (sleeps on the cable), resting, radio window closed:

| Reading | Value |
| --- | --- |
| light-sleep exits over ~100 s | 94-110 |
| wake rate | **0.93-1.01 /s** |
| share of the time asleep | 85-88% |
| sleep length per cycle | 110-1000 ms |
| cause | 100% timer, 0 GPIO |
| speaker resting waits | 1 (it entered the wait once - it does not poll) |
| `timers` (esp_timer table) | **empty**, next alarm `INT64_MAX` |
| `lwip` | next timeout **730-790 ms**, sometimes 250 ms |

So the app's own polls are gone and the chip *still* leaves light sleep once a
second, and the answer is not a task in this repository. With no esp_timer armed
at all, the only thing left that can cap a sleep is a task's deadline, and IDF's
`vApplicationSleep` sleeps for exactly `MIN(next task unblock, next esp_timer
alarm)` (`esp_pm/pm_impl.c`). The tcpip thread is the one holding it: lwIP arms its
cyclic ARP timer for 1000 ms **from now, on every fire**, and does it whatever
the interface is doing - `esp_netif_init()` at boot is enough, no Wi-Fi and no
association required. A deadline re-armed from the moment it fires is why every
cycle is ~1000 ms rather than the half-period average a fixed-phase timer gives.

That is the floor while the network stack exists. **The click is the amplifier
being enabled across it**; the wake itself is a battery cost, not an audio one.
Removing the wake means not having lwIP up while the device rests, which is a
change to the netif's lifetime and needs a Wi-Fi bench to prove uploads still
work - tracked separately.

One more poll of the same class was removed with this: the console's own
`hg-console-wait` task looked for a USB host every 500 ms. A cell never presents
one, so it was **two light-sleep exits a second for the whole life of the
battery**, and it is invisible on a cable because the task deletes itself the
moment a host appears. The app loop now asks `console::start_if_host()` on the
pass it was going to take anyway, so noticing a cable costs no wake.

The `wakes` console verb reports what the firmware actually measured: light-sleep
exits split by cause, how long each sleep lasted, the speaker task's resting
waits, and the time spent asleep. `timers` prints the esp_timer table and the
next alarm, `lwip` prints the network stack's next timeout, `tasks` prints the
FreeRTOS task list. Those four name a wake source; a count alone cannot.

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

(`--project-option` is PlatformIO's; with the venv's `pio` 6.2 pass the flag as
`PLATFORMIO_BUILD_FLAGS="-DHG_BENCH_LIGHT_SLEEP"` instead - the env has no
`build_flags` of its own, so nothing is lost.)

Then `tools/idle_wake_probe.py` does the whole run: zero the counters, `epd idle
on`, wait for the console to go quiet (that is the chip sleeping), rest, reset the
chip over USB, and read the counters back - the rate, the cause, and the range of
sleep lengths. It fails above `--gate` wakes per second.

Bench-only console verbs, in a `HG_BENCH_LIGHT_SLEEP` build: `hold` (take a
`NO_LIGHT_SLEEP` lock so the console can be read after the radio window closes -
`timers`, `tasks` and `pm` are otherwise only readable in the first seconds of a
boot, which is the wrong moment), and `amp on|off` (force the amplifier's enable,
which is the only way to put the pre-fix resting state back and measure it).

Two bench traps, both met while writing this:

- A bench build that sleeps on the cable can wedge the USB link: the port stops
  accepting control transfers and `esptool` answers `EPROTO`. Toggling the port's
  `authorized` file catches one of the chip's awake windows and brings it back.
- **The cable is not the cell, and the speaker cannot be heard through it.** With
  the bench build sleeping at 1/s and the amplifier forced on, eight 2.4 s takes
  from the USB camera's microphone (`plughw:2,0`; a take returns ~6 s of the 8
  asked for, so record many and analyse each) show no once-a-second transient at
  all: median autocorrelation peak r 0.10 in both the enabled and disabled state.
  On USB the load steps land on the charger's output, not on the cell's rail. So
  the click itself can only be recorded with the unit off the cable; what this
  bench proves is the wake rate sound, not the audio.
