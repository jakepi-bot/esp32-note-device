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
| `ui_render.*` | the layout: four zones, bars, icons, all nine screens | `tests/test_notes_screens.cpp` |
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

* **Full** — every screen change, a list rotation, a change to the Home hero,
  and every eighth consecutive selection-bar move (§15.3);
* **Partial** — bar moves, counters, a mark appearing, the playback position;
* **None** — a publish that changed nothing, and a hold on the pairing ask.

`ui_device.cpp` translates that into `request_full_refresh()` before the flush.

## The recording face follows the runtime

`ScreenApp` does not decide whether a capture is running: `Runtime::recording()`
does, and `NotesUi` asks it on every tick and on every runtime change
(`sync_capture`). Two moves come out of that:

* a capture that ends with no key — a console `notes stop`, a microphone or card
  error, the runtime's own abort — takes the glass off the recording face, so the
  counter cannot run on over a note already on the card;
* a capture no key started — a console `notes record` — puts Home onto the
  recording face, with the runtime's own elapsed second as the counter's origin.
  Any other screen keeps its place: a person reading the list is not yanked.

When the runtime refuses a recording at the press (`no microphone`, `no card`, a
note already in flight), the screen machine undoes the entry (`decline()`), so
the recording face is never painted for a capture that does not exist, and the
adapter logs a warning rather than falling through quietly.

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

`hg_notes_ui_tests` is 25 cases: the hero precedence chain, the band and line
never restating it, the menu's right column, list order and marks, the window
the app asks for being the window it draws (every cursor step and rotation), an
empty card, every screen reached by keys, the refresh class of a screen change /
bar move / eighth move / rotation / hero change / quiet publish, delete through
a real `Store` in a temp directory, playback of a real WAV, and an ink-bounds
check of all nine screens at 1 bit (x 4..196, y ≤ 186). Six more cover the
refresh policy added after QA: a note committed after boot reaches the next
frame, a key press reads the card before the screen machine acts, the boot frame
knows the card it booted with, the link/clock/card-space facts reach About, the
list title says `128+` when it is a window, and an upload's progress and failure
reach the glass.

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
