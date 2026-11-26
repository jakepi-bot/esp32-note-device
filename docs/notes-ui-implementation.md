# The notes UI, as built

This is the map for `docs/notes-ui-spec.md`: the spec says what the glass shows,
this says which file makes it so, and how each rule is verified without a
camera. Everything here is in `firmware/esp32/main/notes_app/` unless it says
otherwise.

| File | What it owns | Tested by |
|---|---|---|
| `ui_keys.*` | the key map: one press, one `Action` | `tests/test_notes_ui.cpp` (8 cases) |
| `strings.hpp` | every English word the UI draws | the renderer's ink check |
| `ui_view.*` | the copy: hero precedence, the one line, list rows and marks | `tests/test_notes_screens.cpp` |
| `ui_render.*` | the layout: four zones, bars, icons, every screen | `tests/test_notes_screens.cpp` |
| `ui_app.*` | the screen machine: `Action` → screen + effect + refresh class | `tests/test_notes_screens.cpp` |
| `ui_cache.*` | the card as the screens see it, read on events only | `tests/test_notes_screens.cpp` |
| `wav_player.*` | the file half of playback | `tests/test_notes_screens.cpp` |
| `../notes_app/ui_device.*` | the adapter: ports, panel, recording, playback | device build + the console's `ui` |

## The layers, and why there are four

```
key edge ─► ui_keys.cpp ─► Action ─► ui_app.cpp ─► screen + Effect + Refresh
                                        │
                                        ├─► ui_cache.cpp   (reads the card, on events)
                                        ├─► ui_view.cpp    (facts → copy)
                                        └─► ui_render.cpp  (copy + geometry → pixels)
                                        │
                              ui_device.cpp ─► panel, codec, card, radio
```

The split is the spec's §13.1 taken literally: the key rules are pure and test
in microseconds, the screen machine is pure and tests against hand-built
snapshots, and only the adapter knows a port exists. That is also what keeps the
draw path off the card: `ui_cache` is called when something happened, and
`ui_render` reads nothing but the view it was handed.

## The refresh classes

`ui_app.cpp` returns a `Refresh` with every outcome, from the spec's transition
table:

* **Full** — the first frame after boot, the sleeping face, the busy face of a
  wipe or a format, the recording face, the player, the pairing ask, the pages a
  port owns, a list rotation, a change to the Home hero, every eighth
  consecutive selection-bar move (§15.3), and the fourth hand-over in a row;
* **Partial** — a hand-over between the UI's own pages (Home, the menu, the
  list, a note, and the questions they ask), bar moves, counters, a mark
  appearing, the playback position;
* **None** — a publish that changed nothing, and a hold on the pairing ask.

A hand-over is a page replacing a page. It is a partial because the driver hands
the panel both frames and the partial waveform drives every pixel to its new
state in one 589 ms refresh, without the flash (§15.1), and
`ScreenApp::handover_frame()` caps
a run of them at three so no page carries more than three hand-overs of
residue: the fourth is the full refresh that clears the glass.

`ui_device.cpp` translates that into `request_full_refresh()` before the flush.

## The band, and the flicker budget

Zone A draws the band's facts and owns the only polling the screens do
(`ui_state.cpp`, `ui_render.cpp` `draw_band`, `notes_app/ui_device.cpp`
`read_facts`). The spec's §3.2 is the rule; the measured behaviour is:

| Fact | Where it comes from | What it takes to repaint |
|---|---|---|
| queue count | `Runtime::refresh_counts()`, on a count that moved | a count that actually changed |
| clock warning `NO TIME` | `ports_.system->wall_time()` → `Facts::clock_set` → `Snapshot::clock_set` | the set/unset answer changing (boot or the first SNTP sync), on Home; a minute rollover never repaints |
| Wi-Fi | `Wifi::current()` | the association coming or going |
| Bluetooth | `PhoneLink::radio_up()` / `phone_window()` → `Facts::bt` → `Snapshot::bt` | the radio's window coming or going |
| battery | `hgp::battery_percent` over the port's `PowerStatus` and `board.battery_curve`, every 60 s — the same call `Runtime` prints in `notes status` | a ≥ 2 % step, or any step once 5 minutes have passed |
| card free space | `statfs`, every 60 s | a move of ≥ 1 MiB |

Three of those rules are the flicker fix, and each is one line of intent rather
than a timer:

* `UiState::set_facts` compares instead of publishing. A one-percent battery move
  is the ADC's own noise on this board, so it is neither drawn nor adopted; the
  glass keeps the level it drew until a 2 % step or the five-minute window.
* The clock's minute is not a fact the band compares any more: the band draws no
  time, and only the `clock_set` boolean can move it, so minute rollover costs no
  frame and no wake (Jake, 2026-11-26).
* `kFactsMs` is 10 s (it was 1 s). The card's free space and the battery keep
  their own 60 s path, and `Snapshot::bt` rides the 10 s one.
* `set_facts` returns before anything is adopted while `capture_recording_` or
  `Snapshot::recording` is set, so a capture's only repaint is the counter — the
  first poll after the stop is what brings the band up to date, without a frame
  in the middle of the note.

`ui` answers `..., clock set, ble off|advertising|connected, frames N`: the mark
is the radio's answer in the same spelling `radio` prints, and `frames N` counts
every panel refresh since boot, which is how "nothing repainted" is measured over
the console without a log line per refresh. On the board, before the clock was
removed, two minutes on Home moved it by exactly two (the clock's minutes); with
the band drawing no time an idle Home moves it by zero. Two minutes in a menu by
none, and a 95 s capture by three, 29.4 s apart.

## The recording face follows the runtime

`ScreenApp` does not decide whether a capture is running: `Runtime::recording()`
does, and `NotesUi` asks it on every tick and on every runtime change
(`sync_capture`). Three moves come out of that:

* a capture that ends with no key — a console `notes stop`, a microphone or card
  error, the runtime's own abort — takes the glass off the recording face, so the
  counter cannot run on over a note already on the card;
* a capture no key started — a console `notes record` — puts Home onto the
  recording face, with the runtime's own elapsed second as the counter's origin.
  Any other screen keeps its place: a person reading the list is not yanked;
* a tap the runtime kept for later — an upload was in flight, so `start_note`
  armed a pending recording instead of starting one — draws the recording face
  with `QUEUED` / `FOR THE UPLOAD` where the counter would be, and the footer
  `BOOT: cancel`. `Runtime::recording_queued()` is the third fact the adapter
  publishes; when the upload ends the runtime starts the capture and the same
  face stops saying `QUEUED` and takes the runtime's second as the counter's
  origin. The counter is not drawn before the capture exists, and a tap on the
  queued face cancels the wait.

When the runtime refuses a recording at the press (`no microphone`, `no card`),
the screen machine undoes the entry (`decline()`), so the recording face is never
painted for a capture that does not exist, and the adapter logs a warning rather
than falling through quietly. A queued recording is not a refusal: the press
stands, and the adapter asks `sync_capture` before painting so the first frame is
the queued face rather than a counter over a note that is not being made.

## Playback

`wav_player.cpp` reads the note's own `recording.wav` (chunk walk, so a `LIST`
chunk does not break it) into a fixed 100 ms buffer; `ui_device.cpp` runs that
in its own task and publishes the position through an atomic, which the app task
picks up and draws at 5 s. A stop is a flag the task sees within 20 ms, so a key
press never waits for the codec.

## What a reviewer can check without hardware

```
$ cmake -S firmware -B /tmp/hgb && cmake --build /tmp/hgb -j8
$ ctest --test-dir /tmp/hgb --output-on-failure
```

`hg_notes_ui_tests` is 31 cases: the hero precedence chain, the band and line
never restating it, the menu's right column, list order and marks, the window
the app asks for being the window it draws (every cursor step and rotation), an
empty card, every screen reached by keys, the refresh class of a screen change /
bar move / eighth move / rotation / hero change / quiet publish, delete through
a real `Store` in a temp directory, playback of a real WAV, and an ink-bounds
check of every screen at 1 bit (x 4..196, y ≤ 186). Six more cover the
refresh policy added after QA: a note committed after boot reaches the next
frame, a key press reads the card before the screen machine acts, the boot frame
knows the card it booted with, the link/clock/card-space facts reach About, the
list title says `128+` when it is a window, and an upload's progress and failure
reach the glass. Six further cases cover the Phone screen (t_9972e2a1): the
menu row opens its own screen instead of flashing, the port's phase and countdown
draw, the countdown repaints on the 5 s beat, each of the three refusals is a
word on the glass, the window closes on its own, and PWR or a hold ends it.

## The window rule

The screens read note stubs out of `Snapshot::notes`, and only the window
`ScreenApp::window()` names is ever filled - six rows for the list, one for a
note's own screen, the confirm and the player, one for Home's LAST line, none
for the menu, Recording, Pairing and About. `ui_device.cpp` asks the app after
every key and re-fills only when `(first, rows)` moved, which is what keeps the
draw path off the card while still letting the cursor rotate the list. Computing
that window in the device from the screen alone is the bug the nineteenth case
pins: the stub for a rotated row, or for a note opened past row six, is not
there, and the frame draws blank.


On the device, `hg> ui` prints the screen and the counts, and `hg> screen` dumps
the panel image, which is how a transition's ghosting is measured.
