# Using the notes device

What the glass shows, what each key does, and what the device does with the card.
The rules here are the ones in the firmware: the key map is
`firmware/esp32/main/notes_app/ui_keys.cpp`, the copy is `ui_view.cpp`, the layout
is `ui_render.cpp`. The full specification is
[the notes UI spec](notes-ui-spec.md); the file-by-file map is
[the notes UI, as built](notes-ui-implementation.md).

The device is a notes recorder and nothing else. It does not talk to Hermes, run
turns, or speak replies - see [the README](../README.md#what-it-does-not-do).

## Keys

| Gesture | Home | Recording | Menu | Note list | A note | Delete? | Playing | Pairing ask | About |
|---|---|---|---|---|---|---|---|---|---|
| **PWR** tap | power off | power off | select | open | select | keep | power off | refuse | power off |
| **BOOT** tap | record | stop and keep | next | next | next | keep | stop | allow | nothing |
| **BOOT** hold 2 s | menu | discard | back | back | back | **delete** | stop | nothing | back |

**PWR** held for 2.5 s is the battery latch itself and works on every screen: it
cuts the power. A PWR tap is the graceful path, which commits a recording in
progress and flushes the card first.

## Home

The resting face, and what it returns to when a screen is left.

```
┌───────────────────────┐
│ ▂▄▆█        12:04  ▮  │   the queue band, the last synced minute, battery
│                       │
│        ALL SENT       │   hero: the single most important fact
│     LAST 14:02        │   one line of context
│                       │
│   BOOT: record        │   the key legend
└───────────────────────┘
```

- **Band** - `N TO SEND` or `ALL SENT`, blank when the hero already said it.
- **Hero**, highest priority first: `NO CARD`, `LOW POWER`, `UPLOADING`,
  `NO SYNC` (a batch failed), `OFFLINE` (notes waiting and no station), `NO TIME`
  (clock unset), `READY`.
- **The line under the hero** never repeats the band: `PLUG IN SOON`,
  `NOTHING LOST` (no card, so nothing can be deleted), `3 OF 7 SENT` during an
  upload (with a progress bar), `LAST 14:02` (the newest note's minute), or
  `BOOT TO RECORD` on an empty card.
- **The clock** sits in the band, `HH:MM`, blank until the device has had the
  time once (SNTP, or `clock set` from the console). It is the one fact allowed to
  repaint a resting face: it draws when the minute changes and only on Home, so
  a minute costs one partial refresh (sixty an hour, which is why it is drawn at
  scale 1 and only here). A minute that turns over in a menu is drawn by the
  frame that returns Home.
- **Bluetooth** takes the Wi-Fi glyph's box in the band while the radio has a
  window open: a thin stem-and-lobes rune while it advertises, a 2 px stem while
  a phone is connected. With the radio off the box is the Wi-Fi glyph again, so
  the band never shifts a pixel.
- **Battery** is the level the console prints: the port's own percentage if it
  has one, else the board's measured discharge curve, else the linear 3.30-4.15 V
  fallback, all through one conversion (`notes_app/battery_facts.hpp`) so `notes
  status`/`battery` and the glass cannot disagree. On a cable the shell carries a
  bolt instead of a level; with no reading at all it carries `?`, which is
  "unknown", never 0 %. A level that moved by 2 % or more repaints at the next
  battery poll (once a minute), a 1 % step waits for the five-minute check, so
  the ADC's noise does not cost frames.

## Recording

BOOT-tap on Home starts a note. The screen is one word, `RECORDING`, with an
elapsed counter that starts at `0:00` and moves every 30 s; the footer says
`BOOT: stop` and `HOLD: discard`. BOOT-tap keeps the note, BOOT-hold throws it
away. Recording continues while the radio does whatever it was doing; nothing on
this screen waits for the network. A tap that lands while a note is already
uploading is kept, not dropped, but it is not started mid-transfer either: the
microphone's samples reach the app task, and that task sits inside the upload's
socket writes for seconds at a time, so a capture begun then stores a fraction of
what the mic delivered (measured: 21 s of a 36 s capture). The glass says so
instead - `RECORDING` with `QUEUED` and `FOR THE UPLOAD` where the counter would
be - and the capture begins by itself on the pass the note in flight finishes,
not the whole queue (`notes status` reads `mode syncing+record-queued` while it
waits, then `recording` once it starts). A BOOT tap on the queued face cancels
the wait; nothing has been recorded yet, so there is no note and no second frame.
The screens follow the runtime, not the other
way round: a capture the console ends (`notes stop`) takes the glass back to Home
instead of leaving a counter running over a note already on the card.

A note runs until the card fills, not for a fixed few minutes, and one note stops
at 2 GiB (12.4 h at 24 kHz PCM16 mono) so the file stays inside what FAT32 and the
RIFF size fields hold. `note_max_s` (console `set note_max_s`, 10..86400) is the
only shorter limit, and its default is 86400, so nothing stops a note early out of
the box; the counter's second line shows the cap, `OF 24:00` at the default.

A note is written audio first, then `note.json`, so a power cut never leaves a
note that looks complete but has no audio.

## Menu

BOOT-hold on Home opens it. PWR selects; BOOT moves down; BOOT-hold goes home.

```
MENU
Notes        12
Sync now      3
Phone
Wi-Fi       off
About
Back
```

- **Notes** - the list. The number is what the card holds.
- **Sync now** - open a Wi-Fi window, upload everything pending, return to Home
  with the result. The number is how many notes are waiting.
- **Phone** - open the Bluetooth window for the phone page; `port_ble` owns the
  glass until the window closes.
- **Wi-Fi** - open the phone's setup page (`on`/`off` is the station state).
- **About** - battery, firmware version, device id, notes, card free space.
- **Back** - Home.

## The note list

Newest first, six rows on the glass, one line each: `21 Nov 14:02`. A `*` marks a
note still to send, `!` a note the server refused; the title line explains the
mark (`* TO SEND`, `! FAILED`) or shows the count. An empty card reads
`NO NOTES YET`.

PWR opens the note under the selection; BOOT moves down; BOOT-hold returns to the
menu. The on-screen window stops at 128 notes and the title line then reads
`128+`; every note is still on the card, and the list starts at the newest.

## A note

`NOTE`, the note's day, `<time> · <duration>`, and its state (`to send`, `sent`,
`failed`), with three actions: **Play**, **Delete**, **Back**. PWR selects, BOOT
moves.

- **Play** sends the note's own WAV to the speaker. The position is drawn every
  five seconds, not every second; BOOT-tap stops.
- **Delete** asks first. `DELETE?` with the note's day on the screen; BOOT-hold
  deletes, BOOT-tap (or PWR) keeps. Delete removes the card's copy only. A note
  already in the vault is untouched.

## Phone

The menu's **Phone** row has its own screen, and the Bluetooth window lasts as
long as it does. `PHONE`, the device's name (what the phone's chooser lists),
the window's state, a countdown, and the pairing table (`2 OF 3 PAIRED`):

- **ADVERTISING** the controller is coming up, **WAITING** it is on the air and
  no phone is connected, **CONNECTED** a phone is on the link.
- The window is **five minutes**. A phone that is connected holds it open: the
  deadline is pushed a minute out on every pass while the link is up, so it never
  ends under a phone, and it closes within a minute of the phone leaving.
- **WINDOW CLOSED** when it expires on its own, with `Radio off` under it and the
  radio back to rest (plus the policy's 20 s closing grace). The screen stays
  until a key, because an e-paper page that vanished would not say what happened.
- **PWR closes the window** (and the radio goes off with it), **HOLD goes back to
  the menu**, **BOOT does nothing**.
- A window that cannot open says why in the state slot instead of flashing back
  to the menu: **PAIRING FULL** (every bond slot taken), **RADIO BUSY** (the
  controller did not start), **BLE OFF** (a build with no Bluetooth). The same
  reason is a warning in the log.

The pairing ask is a full screen: `Same as phone?` with the six-digit code,
`BOOT: allow` and `PWR: refuse`. **BOOT allows, PWR refuses, and 20 seconds of
neither refuses.** Confirm the code only when the board and the phone show the
same one. Pairing is remembered.

The board advertises only inside a window: boot with notes waiting, the Phone
menu row, or shortly after a recording. At rest the radio is off, which is what
lets this board sleep.

## About

Battery percentage and whether external power is present, the firmware version,
the device id (`hg-...`), the note count and the card's free space. Firmware
0.7.1 reports `Card free` as `0.0G`; that is a known defect (QA D3), not a card
that is full - `sd` on the console prints the real figure.

## Card layout and retention

```
notes/
  pending/<note_id>/recording.wav + note.json
  sent/<note_id>/
  failed/<note_id>/
```

- The upload moves a note from `pending/` to `sent/` only after the server
  answers 2xx (or `200 duplicate`); a terminal 4xx moves it to `failed/`.
- Synced notes are deleted after 14 days, and oldest-first when the card is under
  10% free. `pending/` and `failed/` are never deleted by age, and never by size
  either: a note the hub has not taken is not a candidate however large it is
  (a 1.5 GiB pending note beside an older synced one is a host case).
- A note is capped when it starts: the setting (`note_max_s`, 24 h out of the
  box), the 2 GiB per-note ceiling, and the card's free space less a **256 MB
  reserve**, whichever is smallest. The recording face says which one it got
  (`OF 1:30·2.2G`), so a note is never the write that fills the volume. A card
  with less than the reserve free starts no note at all (`notes record` answers
  `SD almost full, no synced notes to delete`); the 10 % above is the rule that
  decides when synced notes are pruned, not whether a note may start, so a big
  card at 9 % free still records what it has room for.
- The free space is re-read every 30 s while a note runs (the same tick the
  counter moves on, no second timer). If the card fills anyway - another writer,
  a mount that over-reported - the note **commits** with the audio it holds: the
  RIFF header is rewritten with the real byte count and the log says
  `(the card filled: kept what it holds)`. A note that reaches its own budget
  stops the same way and logs `(the note's budget reached)`.
- `notes list` prints each id with its `duration_seconds` (`<id>=5400`), so a
  note past 100 minutes reads its real length; the note screen prints it as
  `14:02 · 1h30` and the counter as `1:30:00`. The list shows the newest eight
  ids per state - the cap is there to keep one console line readable, so it must
  not hide the note just recorded.

## Radio and power

- Wi-Fi stays on by default (`wifi on`, the setting stored in NVS, remembered
  across a power cycle): the station associates at boot and keeps its link, so
  the hub and the phone's page can always reach the device. `wifi off` (console
  or `set wifi off`) goes back to windows only: a window opens on boot with notes
  waiting or the clock unset, after a note, on **Sync now**, and for the setup
  page; it closes when the queue is empty and the clock is set, and a 15-minute
  retry clock reopens it while work remains.
- SNTP runs only while the station holds an address.
- Bluetooth advertises only inside its own windows: the two-minute pairing
  window, the five minutes the Phone screen owns (held open by a connected
  phone), the minutes after a note, the retry while notes wait, and an explicit
  `phone wake`. At rest the controller is off.
- The radio can be stopped and started again as often as the windows ask: the
  NimBLE handles the host allocates for the app are cleared when the controller
  deinitialises, so the second window after the radio has rested (a note saved,
  `phone wake`, the Phone screen) allocates fresh ones. Without that clearing the
  second window memsets through a freed pointer and the device panics and
  reboots - seen at 152 s on `phone wake` after the boot window closed, and after
  a note was saved, fixed in `port_ble.cpp`'s `stop_radio()`.
- A PWR tap powers off gracefully: the recording is committed, the upload in
  flight is cancelled, the card is flushed and released, the station is stopped,
  and the battery latch drops. On USB the card is mounted again, because the
  board stays up.
- This board has no hardware power switch: the battery latch (GPIO 17) is what
  keeps the cell connected, so press and hold PWR for about two seconds to start
  it on battery. The firmware drives the latch high as soon as it runs, keeps it
  high every pass, and ignores a PWR level that sits low through boot or through
  a change of power source, so an unplug cannot read as a finger and power the
  board off.

## Console

Over USB at 115200: `notes status|list|record|stop|sync|poweroff|mount|format|prune`,
`screen` for the panel image as hex, `about`, `battery`, `clock`, `power`, `radio`,
`sd`, `wifi [on|off]`, `keys`, `ui` (`ui home` forces a full-refresh hand-over,
`ui press boot|hold|pwr` presses a key from a host with no finger on the board;
the plain `ui` line also answers `ble off|advertising|connected` for the band's
radio mark and `frames N`, the panel refreshes since boot, which is how an idle
face is shown never to redraw),
`key boot|hold|pwr` (the same press, answering with the screen it left behind, so
`@ok boot tap -> screen home` is a tap the runtime refused and
`@ok boot tap -> screen recording` is one it took), `phone pair|wake|allow|refuse`,
`wifi-setup [stop]`, and the usual `set`/`get`/`diag`/`log`/`help`. The `screen`
verb is how a transition's ghosting is measured without a camera.

## Known limits

- No on-device OTA: updates go over USB with `pio run -e notes -t upload`.
- The Wi-Fi setup page asks for a Hermes address because it is the shared port's
  page; only the host is used (it posts to `http://<host>:8766/notes/v1`). `set
  notes_url` overrides it.
- Two notes recorded in the same minute are indistinguishable on the delete
  confirm; the hold and the list order are what protect them.
- The full-refresh duration of this panel has not been measured; every screen
  change is budgeted at about a second.
- The upload planner waits for the capture, and a capture waits for the note
  being uploaded: a tap during a sync is queued (`QUEUED`/`FOR THE UPLOAD` on the
  glass, `mode syncing+record-queued` at the console) and starts when that one
  transfer ends, so the wait is one transfer, not the whole queue. A failed
  attempt releases the tap as well - the runtime is idle between retries, and the
  capture takes that pass; a very large note in flight is still a long wait for
  the next capture.
