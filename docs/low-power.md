# Low power on the e-paper notes device

What the firmware spends energy on, what was removed or made rarer, and what a
reviewer can check without a board. Numbers are from the host tests and the
ESP-IDF build in this change's workspace; the board measurements are listed at
the end as the checks the hardware pass still owes.

## The starting point (measured in the tree this change is based on)

| Item | Before |
|---|---|
| App loop wake-ups, resting | 100/s: `events::receive()` always waited 10 ms |
| UI model rebuilds, resting | 10/s: `App::tick()` rebuilt the whole `UiModel` every `kFrameMs` (100 ms) |
| Notes card scan | every 5 s, plus a full `pending/` directory walk on every loop pass while the link was up |
| Doze / light sleep | **never**: `App::power_tick()` gated dozing on `DisplayInfo::has_backlight`, and the e-paper panel reports none. The `hg-awake` PM lock was therefore held for the life of the device, so the board's light-sleep configuration (`board.light_sleep = true`, `CONFIG_FREERTOS_USE_TICKLESS_IDLE`) never took effect |
| Keys | polled from the app loop, ~100 Hz, with 3 ticks of debounce |

The doze line is the important one. The board was built to light-sleep
(`boards/waveshare-epaper-154/sdkconfig.defaults` enables power management,
tickless idle and the light-sleep callbacks), but the app's own gate never let
it, so the chip stayed at full clock with a 100 Hz timer.

## What changed

1. **The resting panel is a dark state (core).** `DisplayInfo::bistable` marks a
   panel that keeps its image unpowered. `App::power_tick()` now treats
   "bistable panel, a board module owns the frame (`App::rest_screen`), idle,
   on battery" as the e-paper equivalent of a backlit screen the timeout turned
   off, and dozes. Backlit boards take exactly the same path as before.
2. **The keys wake the loop instead of the loop waiting for them
   (port).** ModeRouter installs an any-edge interrupt on PWR and BOOT; the
   handler posts an allocation-free `EventType::KeyEdge` with
   `xQueueSendFromISR`. `ModeRouter::fast_poll()` then keeps the loop at its
   original 10 ms cadence while a press is being debounced or held, and returns
   true unconditionally when no interrupt could be installed (the old polling
   behaviour is the fallback, not a lost feature).
3. **Resting wait (port).** With the status screen owning the glass and the
   notes mode idle, the loop blocks 250 ms per pass instead of 10 ms
   (`App::kRestWaitMs`), and 5 ms while `fast_poll()` says a press is in
   flight. A doze still wins at `App::kDozeWaitMs`.
4. **Model rebuild at rest (core).** `App::tick()` rebuilds the model every
   `App::kRestFrameMs` (1 s) while a board module owns the frame, instead of
   every 100 ms. Every state change still calls `update_model()` directly, so
   the screen never waits for this clock to show news; the clock minute is at
   worst one second late on a panel that takes 300 ms to refresh.
5. **Light-sleep wake for the keys (port).** While dozing, the key pins keep
   their pull-ups (`gpio_sleep_sel_dis`) and become low-level wake sources
   (`gpio_wakeup_enable` + `esp_sleep_enable_gpio_wakeup`); the touch controller
   already had this. A level interrupt and the edge handler cannot share a pin's
   type register, so the edge handler is parked for the doze and restored on the
   wake hook (`EspSystem::on_doze_start`/`on_doze_end`).
6. **Card scans (port).** The counts scan stays at 5 s while a note is pending or
   failed, and drops to 30 s when nothing is queued (`kIdleScanMs`). Every
   writer (a finished note, the phone's transfer, the console) recounts at once,
   so the slow clock cannot miss a note. `begin_sync()` also returns before
   walking `pending/` when the last count says there is nothing to send; it used
   to walk the directory on every loop pass while online.

## What was deliberately **not** touched

- The button remap (PWR = Home, BOOT = record/talk, hold-to-talk), the home
  screen's TALK / pairing-ask strip, and the SD free-space footer are unchanged;
  the only port change near them is how their key edges wake the loop.
- The status screen's renderer and its signature change check already avoided
  identical repaints, and the EPD driver already coalesces bands and enforces a
  300 ms gap between refreshes. Nothing there needed changing.
- Retention behaviour is not altered by this change. **Note for the reviewer:**
  the retention commit (`d2a8106`, "the card deletes its synced notes by itself
  before it fills up") lives in the sibling `hermes-gadget-sdk` checkout, not in
  this `esp32-note-device` tree, whose HEAD (`eb7cf0b`) carries the button and
  home-screen work. This refactor keeps whatever is in the tree it builds and
  removes no retention code; the two trees must be reconciled before the
  integration QA pass. That split is recorded in the task handoff, not silently
  papered over here.

## Measured result

| Item | Before | After |
|---|---|---|
| Resting loop wake-ups | 100/s | 4/s (250 ms); 200/s only during a 400 ms window after a key edge |
| Resting UI model rebuilds | 10/s | 1/s |
| Idle card scans | every 5 s + a `pending/` walk per pass | every 30 s, no `pending/` walk when empty |
| Doze | never | when the status screen rests, the device is idle/online and on battery |
| Firmware image | 2,040,224 B (`hermes_gadget.bin`, 50% of the app slot free) | 2,041,888 B (still 50% free) |

Host tests: 8/8 suites pass, including a new case that drives the bistable
doze ("a resting bistable panel lets the chip sleep with no backlight and no
timeout": dozing on battery, USB ends it).

## The 24-hour idle budget: verdict

The requirement for this device is **at least 24 hours idle on one charge**: the
screen resting, the radio quiet, nothing queued. That is a single division,
`hours = capacity / average current`, and it needs three numbers. Two of them have
never been measured, so this section keeps what is recorded apart from what is
inferred, and states the verdict both ways instead of borrowing confidence from the
cable.

### The three inputs

| Input | Value | Source | Status |
|---|---|---|---|
| Battery runtime on the cell | **~6 h** at ~40 mA | the owner's own use; neither the build nor the date is recorded | observed |
| Rest current on the cell | **never metered** | every reading on record is USB-side (see below) | unknown |
| Cell capacity | **not recorded** | the only cell label in [hardware verification](hardware-validation.md) (`SKU 33228 3.7V 1000mA`) belongs to the Touch-LCD-1.54, a different board | unknown |

6 h at 40 mA is 240 mAh, so the cell is the ~250 mAh class. That is an inference
from a runtime, not a label read. Nothing below depends on it being exact: the
requirement fixes the current, `I = capacity / 24`, and every figure scales with
the measured capacity when the label is read.

### The arithmetic

| Average current | 240 mAh | 500 mAh | 1000 mAh |
|---|---|---|---|
| 40 mA (the observation) | 6.0 h | 12.5 h | 25.0 h |
| 22 mA (datasheet modem sleep, 80 MHz) | 10.9 h | 22.7 h | 45.5 h |
| 10 mA (the threshold at 240 mAh) | 24.0 h | 50.0 h | 100.0 h |
| 3 mA | 80 h | 167 h | 333 h |
| 1 mA (the target in the [UI spec](notes-ui-spec.md)) | 240 h | 500 h | 1000 h |
| 0.38 mA (light-sleep floor, below) | 632 h | 1316 h | 2632 h |

Clearing 24 hours takes an average of **10.0 mA at 240 mAh** (10.4 mA at 250 mAh,
20.8 mA at 500, 41.7 mA at 1000). At the observed 40 mA the device runs about 6 h
on the inferred cell: four times over budget.

### Peaks do not decide this; the rest state does

A realistic day — three recordings of 30, 45 and 75 s, three uploads of 20, 25 and
35 s, eleven key presses, 15.8 h on and the rest with the latch cut — comes to about
8 mAh of charge at generous currents (recordings 50 mA, uploads 150 mA, an awake
screen 25 mA). The other ~230 mAh of a 240 mAh cell is spent resting. The
requirement is therefore a statement about the resting current, and the day's peaks
are noise against it. With the cell off for 8 h of the 24, the awake budget is
looser (14.6 mA); the requirement is 24 hours idle, so **10 mA is the number the
verdict uses**.

### Why the USB figures cannot be used

The meter readings on record are 39 mA with the radio off (the power-management
comment in `firmware/esp32/notes_app/main.cpp`, meter 2026-11-23), 35 mA with the
panel rail down, and 41 mA associated under `MIN_MODEM`. None of them is a battery
number: light sleep never runs while a USB host is attached
(`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`, or the console and JTAG would stop), the USB
PHY alone is worth tens of milliamps, and the notes app refuses to enter the idle
state on the cable at all (`!power_cable`). A cable reading is a different state,
not a pessimistic version of the battery one — derive battery life from it and the
error lands in whichever direction the particular state happens to differ, which is
why the numbers are quoted here only to be excluded.

### The verdict

| | |
|---|---|
| As last observed on the cell | **fails**: ~6 h against a 24 h requirement |
| The current firmware | **undetermined**, on one missing number: the rest current on the cell. The idle state and the doze path landed on 25 November (`c73a12a`, `d9d7c79`) and no runtime has been recorded since, so the ~6 h figure may predate them |
| What has to be true | an average of at most `capacity / 24` — 10.0 mA on the inferred cell |
| Is that reachable | on paper, yes (below); as a measurement, not yet |

### The light-sleep dependency

The verdict does not hold with the firmware that produced the 6 hours. It becomes
true only once the doze path and the idle state are both live at rest on the cell:
light sleep between a 10 s wake (`kIdleWaitMs`), the panel rail down, the station
stopped, and no periodic refresh on the resting face — which is why that face has
no clock ([UI spec](notes-ui-spec.md), decision 3).

Projected at that rest state: the chip's light sleep is 240 µA and the 8 MB
8-line PSRAM adds 140 µA at 3.3 V or 200 µA at 1.8 V (ESP32-S3 datasheet v2.2,
Table 5-10 and its PSRAM note), so **0.38-0.44 mA**, plus a negligible cost for one
wake per 10 s. On the inferred 240 mAh cell that is **540-630 h**; even a pessimistic
1 mA gives 240 h and 3 mA gives 80 h. Every one of those clears 24 h by at least 3x,
projected.

The projection fails in exactly one way, and it is the case to measure: an
associated station. Modem sleep is 13-19 mA at 40 MHz and 22-36 mA at 80 MHz
(datasheet Table 5-9), so 240 mAh at 22 mA is 10.9 h — under 24 h even at the good
end of the range. The notes design stops the station at rest and reopens a two
minute window every 15 minutes only while notes are queued; a queue that is empty
but still holds a window, or a window that never closes after an upload, breaks the
verdict without breaking the sleep. That is what the meter run has to catch, and it
is the whole difference between "light sleep landed" and "the device meets 24 hours".

### What is still open

1. **Read the cell label** (mAh, nominal voltage, part). Every figure here scales by
   it, and [hardware verification](hardware-validation.md) carries the fill-in block
   for it. One minute with the case open.
2. **One rest reading on the cell**, radio off and rail down, taken on a VBUS-cut
   cable so the console survives the power cut and `power` answers `supply battery`.
   Predicted 0.38-0.44 mA; anything over 10 mA fails the requirement outright.
3. **One reading with the station associated**, taken right after an upload and held
   for 20 minutes. Predicted 15-40 mA while up; the question is whether it comes
   down. This is the only reading that separates a stopped station from a reopened
   window.
4. **A 24 h discharge log** — `battery` every 30 s overnight — so the verdict rests
   on an observed runtime rather than an integral of assumptions. At the projected
   rest current the whole day costs under 11 mAh of a 240 mAh cell, so the cell
   should end the day barely below where it started; a fall of more than a few
   hundred millivolts means the rest state is not the one this section assumes.
5. **Pin the 6 h observation to a build and a date** (`git rev-parse HEAD` of the
   image that ran it), so the one runtime on record is a commit rather than a memory.
   The stale ~8 h doze budget that used to sit in the integration spec's §9 has been
   replaced with this requirement in the same change.

## Hardware checks the QA pass still owes

These are behaviour changes on a real board and are **not** provable on a host:

1. On battery, leave the device on the home screen: the log shows
   `hg.power: dozing: light sleep allowed` within a few seconds, and
   `awake after N s dozing: light sleep M s (P%)` when something wakes it. Before
   this change the first line never appeared on this board.
2. PWR short press returns home **on the first press** while dozing (the level
   wake must fire before the debounce window). BOOT tap records; BOOT hold talks
   and stops on release, with no visible lag. If a key needs a second press,
   the wake configuration is wrong, not the key map.
3. The screen keeps its image while dozing (it must; the panel is bistable) and
   the clock minute changes within about a second of waking.
4. USB plugged in ends the doze within ~100 ms (the console must stay usable).
5. Record a note while online: the upload starts and the queue drains exactly as
   before (the `pending/` early-out must not hide a note).
