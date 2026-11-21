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
- There is **no clock on the resting face** on purpose: a minute tick would cost
  sixty whole-panel refreshes an hour. Time appears only as a timestamp.

## Recording

BOOT-tap on Home starts a note. The screen is one word, `RECORDING`, with an
elapsed counter; the footer says `BOOT: stop` and `HOLD: discard`. BOOT-tap keeps
the note, BOOT-hold throws it away. Recording continues while the radio does
whatever it was doing; nothing on this screen waits for the network.

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

The pairing ask is a full screen: the phone's address tail and `Same as phone?`,
with `BOOT: allow` and `PWR: refuse`. **BOOT allows, PWR refuses, and 20 seconds
of neither refuses.** Confirm the six-digit code only when the board and the
phone show the same one. Pairing is remembered.

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
  10% free. `pending/` and `failed/` are never deleted by age.
- The device refuses to start a recording on a full card rather than write a note
  the card cannot hold.

## Radio and power

- Wi-Fi is a window, not a state: it opens on boot with notes waiting or the clock
  unset, after a note, on **Sync now**, and for the setup page; it closes when the
  queue is empty and the clock is set, and a 15-minute retry clock reopens it while
  work remains.
- SNTP runs only while the station holds an address.
- Bluetooth advertises only inside its own windows.
- A PWR tap powers off gracefully: the recording is committed, the upload in
  flight is cancelled, the card is flushed and released, the station is stopped,
  and the battery latch drops. On USB the card is mounted again, because the
  board stays up.

## Console

Over USB at 115200: `notes status|list|record|stop|sync|poweroff|mount|format|prune`,
`screen` for the panel image as hex, `about`, `battery`, `clock`, `radio`, `sd`,
`keys`, `ui` (`ui home` forces a full-refresh hand-over), `phone
pair|wake|allow|refuse`, `wifi-setup [stop]`, and the usual
`set`/`get`/`diag`/`log`/`help`. The `screen` verb is how a transition's ghosting
is measured without a camera.

## Known limits

- No on-device OTA: updates go over USB with `pio run -e notes -t upload`.
- The Wi-Fi setup page asks for a Hermes address because it is the shared port's
  page; only the host is used (it posts to `http://<host>:8766/notes/v1`). `set
  notes_url` overrides it.
- Two notes recorded in the same minute are indistinguishable on the delete
  confirm; the hold and the list order are what protect them.
- The full-refresh duration of this panel has not been measured; every screen
  change is budgeted at about a second.
