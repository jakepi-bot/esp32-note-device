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
`hours = capacity / average current`, and all three numbers now exist, each with the
status of its source attached.

### The inputs

| Input | Value | Source | Status |
|---|---|---|---|
| Cell capacity | **320 mAh**, 3.7 V Li-po, EEMB LP402535 | the owner identified it against the purchase listing (ASIN B08215N9R8), 2026-11-26, see [hardware verification](hardware-validation.md#note-taker-battery-cell-capacity) | recorded, but from a listing and not the printed label |
| Idle current, on the cable | **6 mA at 5.15 V**: panel rail down, radio off, microphone suspended, light sleep engaged | inline USB meter, 2026-11-26; the firmware accounted 157 s of light sleep in a 160 s window (98%) | measured |
| Awake current, on the cable | **37 mA at 5.14 V**, BLE advertising | the same run | measured |
| Idle current, on the cell | **never metered**, and not obtainable on this bench | there is no DMM, so there is no shunt in series with the cell (owner, 2026-11-26) | unavailable |
| Runtime on the cell before the power work | **~6 h** | the owner's own use; neither the build nor the date is recorded | observed |

### The arithmetic

320 mAh at 3.7 V is **1.18 Wh**, and hours are watt-hours divided by watts. The
current form is `hours = capacity / average`:

| Average current, at the cell | Hours on 320 mAh |
|---|---|
| 53 mA (what the ~6 h observation implies) | 6.0 h |
| 40 mA (the pre-work estimate) | 8.0 h |
| 22 mA (datasheet modem sleep, 80 MHz) | 14.5 h |
| 13.3 mA (the threshold, `C / 24`) | 24.0 h |
| 8.4 mA (the 6 mA cable reading, converted below) | 38 h |
| 3 mA | 107 h |
| 1 mA (the target in the [UI spec](notes-ui-spec.md)) | 320 h |
| 0.38 mA (the datasheet light-sleep floor) | 842 h |

Clearing 24 hours takes an average of **13.3 mA** on this cell.

### Converting the cable reading to the cell

The meter sits on VBUS, so its numbers are watts, not cell amps: 6 mA at 5.15 V is
30.9 mW and 37 mA at 5.14 V is 190 mW. The check that the conversion is sound is the
runtime it reproduces. 190 mW against a 1.18 Wh cell is **6.2 h**, which is the ~6 h
the owner saw before any of this work, from an observation that predates the meter.

At the idle figure, 1.18 Wh / 30.9 mW is **38 h** with a perfect converter and about
**34 h** at the 89% an LDO from a 3.7 V cell gives. The verdict survives a converter
as poor as 62%. On the cell there is also no USB PHY and no charger in the path, so
the same work costs less power than the VBUS wattage says: the converted hours are
on the conservative side, not the optimistic one.

The 24-hour card's own figure of **53 h** (`320 mAh / 6 mA`) treated a VBUS current
as a cell current. It is 38 h, not 53 h. Both clear the requirement, and the corrected
one is what this section uses.

### Peaks do not decide this; the rest state does

A realistic day — three recordings of 30, 45 and 75 s, three uploads of 20, 25 and
35 s, eleven key presses, on for 15.8 h of the 24 — costs about 8 mAh at generous
currents (recordings 50 mA, uploads 150 mA, an awake screen 25 mA). That is 2.5% of
the cell; the other 97.5% is spent at rest. The requirement is therefore a statement
about the resting current, and the day's peaks are noise against it.

### Why the older cable figures are not the ones quoted

The numbers on record from the 2026-11-23 run are 39 mA with the radio off (the
power-management comment in `firmware/esp32/notes_app/main.cpp`), 35 mA with the panel
rail down, and 41 mA associated under `MIN_MODEM`. They describe a device that was not
sleeping: light sleep does not run by default with a USB host attached
(`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`, or the console and JTAG would stop) and the
notes app refuses to enter its idle state on the cable at all (`!power_cable`). The
2026-11-26 reading is a different measurement of a different state, taken with light
sleep actually engaged and against the firmware that holds the latch through sleep.
Quoting the older figures for runtime is what made the 53 h figure above wrong: the
state, not the instrument, is what changed.

### The verdict

| | |
|---|---|
| As last observed on the cell | **fails**: ~6 h against a 24 h requirement |
| On the best evidence available | **passes**: 34-38 h against 24 h, from a converted cable reading, with light sleep observed at 98% duty |
| On the cell, unmetered | **not obtainable**: there is no DMM on this bench, so a cell-side number cannot be produced at all |
| What has to be true | an average of at most 13.3 mA at the cell |
| The one case that still fails | a station left associated: modem sleep at 13-19 mA is 17-24 h on this cell |

### The light-sleep dependency

Light sleep is no longer a projection for this firmware. The run that took the 6 mA
reading accounted **157 s of sleep in a 160 s idle window**, so what this section
needs is live on the board, not on paper.

The datasheet floor is still worth stating as what a device with no station and no
wake windows could reach: 240 µA for the chip plus 140 µA at 3.3 V or 200 µA at 1.8 V
for the 8 MB octal PSRAM (ESP32-S3 datasheet v2.2, Table 5-10 and its PSRAM note), so
0.38-0.44 mA, which is 727-842 h on this cell. The measured 6 mA is fourteen times
that, and the difference is the USB PHY, the wake windows and the 10 s timer
(`kIdleWaitMs`). The verdict uses the measured number, not the floor.

The verdict still fails in exactly one way, and it is the reading left to take: an
associated station. Modem sleep is 13-19 mA at 40 MHz and 22-36 mA at 80 MHz
(datasheet Table 5-9), so 320 mAh at 13 mA is only 24.6 h and at 19 mA it is 16.8 h.
The notes design stops the station at rest and reopens a two minute window every
15 minutes only while notes are queued; a queue that is empty but still holds a
window, or a window that never closes after an upload, breaks the verdict without
breaking the sleep. That is the whole difference between "light sleep landed" and
"the device meets 24 hours".

### What is still open

1. **Read the printed cell label** (mAh, nominal voltage, part). Capacity is from the
   purchase listing, and a label that disagrees wins: [hardware verification](hardware-validation.md#note-taker-battery-cell-capacity)
   says so in its own words. One minute with the case open.
2. **A cell-side rest reading** needs a DMM in series with the cell, on a VBUS-cut so
   the console survives and `power` answers `supply battery`. The bench has no DMM, so
   this is filed as unavailable rather than pending; the converted VBUS figure above
   stands in until one exists.
3. **One reading with the station associated**, taken right after an upload and held
   for 20 minutes. Predicted 13-36 mA while up; the question is whether it comes down.
   This is the only reading that separates a stopped station from a reopened window,
   and it is the one that can still fail the requirement.
4. **A 24 h discharge log** — `battery` every 30 s overnight — so the verdict rests on
   an observed runtime rather than on a converted wattage. At the projected rest
   current the whole day costs about 12 mAh of a 320 mAh cell, so the cell should end
   the day barely below where it started; a fall of more than a few hundred millivolts
   means the rest state is not the one this section assumes.
5. **Pin the ~6 h observation to a build and a date** (`git rev-parse HEAD` of the
   image that ran it), so the one cell runtime on record is a commit rather than a
   memory. The stale ~8 h doze budget that used to sit in the integration spec's §9
   has been replaced with this requirement in the same change.

The bench procedure and capture scripts for items 2 to 4 are preserved at
`~/share/notes-bench/` (`idle-current/`, `day-profile/`), so they run unchanged the
moment a meter exists.

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
