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
