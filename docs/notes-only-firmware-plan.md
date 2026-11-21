# Plan: a notes-only device

Agreed plan (2026-11-21). Numbers are from the tree as it stands
(`waveshare-epaper-154`, PlatformIO build of 2026-11-21) and from the host tests.

Landed so far: `firmware/esp32/main/notes_app/ui_keys.*`, the screen and key rules,
with eight host cases in `firmware/tests/test_notes_ui.cpp`.

## 1. Where the weight is today

| Slice | Flash (code + rodata) | Notes |
|---|---|---|
| Hermes core (`libcore.a`: App, UI model, settings, i18n, faces, timers, camera, prompts, widgets, TTS glue) | ~203 KB | one generic UI for a dozen boards |
| Port + main (`(exe)`) | ~389 KB | board table, drivers, modes, notes, display, console |
| NimBLE (phone link + note sync) | ~156 KB | needed for the phone path |
| Opus | ~85 KB | only for spoken replies from Hermes |
| WebSocket client | ~7 KB | Hermes live link |
| mbedTLS/PSA rodata | ~69 KB | TLS |
| Image total | 2.05 MB of a 3.9 MB slot | static RAM 88 KB, PSRAM ~3 MB recording buffer |

The device does not feel heavy because of flash. It feels heavy because of two
things: a generic UI that carries turns, prompts, profiles, widgets, faces and
dashboards for boards it is not, and a **persistent Wi-Fi association with the
Hermes gateway** that exists to keep the live link open.

## 2. What the device actually needs

Record, keep, upload, and hand over. Nothing else.

- microSD note store (WAV + `note.json`, `pending/`, `sent/`, `failed/`)
- Wi-Fi STA with the phone-based setup AP, SNTP for the clock
- HTTP `POST /notes/v1` with the HMAC signature (already independent of Hermes live)
- BLE GATT note service (control + data) and the pairing ask (already independent)
- ES8311 microphone capture and speaker playback
- e-paper status face, menu, notes list
- power: latch, battery percentage, graceful off

It does **not** need: the Hermes WebSocket protocol, turns, TTS, prompts, choice
cards, dashboards, profiles, widgets, faces, camera, timers, tailnet, touch,
Opus, the CJK font (already off), or the desktop UI model.

One dependency does stay on the host: transcription. The notes endpoint is the
gateway plugin's, and the container we deployed stages into its inbox. The host
needs Hermes; the device does not.

## 3. Three ways to get there

### A. Trim in place (keep Hermes core)
Add a `HG_NOTES_ONLY` build flag, compile out live mode and the turn paths, keep
the settings menu and i18n.

- Work: 2-3 days, mostly `#if` surgery through `app.cpp`, `app_settings.cpp`, `ui.cpp`.
- Saves: Opus, WS client, maybe 60-120 KB of turn code.
- Keeps: the generic UI, all its states, the core's 8 host-test suites.
- Risk: every future core change has another ifdef; the menu still cannot show a
  list of notes without new core machinery.

### B. A dedicated notes app (recommended)
New app (`firmware/esp32/notes_app/` + `HG_APP=notes`), reusing the hardware
ports and the notes stack, with its own ~5-screen UI.

- Reuse as-is: `port_display_epd` (framebuffer, partial refresh, e-paper timing),
  `port_codec` (mic + speaker), SD mount and `notes::Store`, `notes::Queue`
  (upload planning), the uploader, `notes_ble` (packet/window protocol),
  `port_ble` (GATT table, pairing ask, radio window), `port_time`, `port_power`,
  `port_console`, `port_wifi_setup`, `port_ota` (for USB/manifest updates).
- Drop: `hg::App`, `ui.cpp`, `app_*.cpp`, i18n table (replace with ~40 English
  strings), mascot/face data, Opus, WebSocket client, dashboard/protocol JSON,
  camera, timers, profiles, widgets.
- New: a screen state machine (~600 lines), a notes list/play/delete screen
  (~500), a menu (~300), power/radio schedule (~200).
- Saves: roughly 250-350 KB flash and 40-80 KB internal RAM, and removes the
  whole turn machinery from the device's behaviour. Exact numbers from a built
  variant, not from arithmetic.
- Risk: Wi-Fi setup, pairing ack and upload retry are proven today and would be
  re-hosted; mitigated by keeping the modules and by the existing host tests for
  store/queue/BLE/upload planning.

### C. Hybrid: keep the core library, new app shell
Keep `hg::App` but drive it only through notes screens. Not worth it: it keeps
the weight and adds a second UI grammar.

**Recommendation: B, staged.** The menu Jake wants (browse, play, delete) cannot
live in the core's fixed settings enum anyway, so the "notes screen" work is
required in every option; B is where that work has the fewest moving parts.

## 4. Keys, menu, and the one job

### Key map (proposed)

| Gesture | Home | Recording | Menu / list | Confirm |
|---|---|---|---|---|
| PWR short | power off (graceful) | power off | power off | power off |
| PWR hold | hardware latch off | — | — | — |
| BOOT tap | start / stop note | stop and keep | next item | no / cancel |
| BOOT hold 2 s | open menu | discard the note | select / enter / play | yes / delete |

PWR becomes power only, which is what Jake asked. The cost: PWR cannot be "select"
inside the menu, so selection is the held BOOT. The delete confirmation is the
same hold: *hold to delete, tap to keep*, with the note title on screen.

### Menu

```
Notes (12)      >   list, newest first: date, time, length
Sync now        >   upload every pending note now
Phone           >   pairing window (existing screen)
Wi-Fi           >   network + phone setup
About           >   battery, firmware, device id, SD free
Back
```

Notes list: each row `21 Nov 14:02  0:14  pending|sent`. Selecting a note opens
**Play** (speaker, BOOT tap stops) and **Delete** (confirm). Delete removes the
file from the card only; a note already in the vault stays there. `sent/` notes
are deletable too (that is what retention does automatically today).

### Power schedule

1. Never hold a Wi-Fi association for live: connect on demand (boot with pending
   notes, after a note finishes, from **Sync now**), upload the batch, disconnect.
2. BLE: advertise only in windows (pairing, 30 s after a recording, while
   charging), as `radio_window` already does.
3. CPU: 80 MHz idle, light sleep at rest, GPIO wake on both keys; the panel holds
   its image.
4. Report the duty cycle in `status` (awake ms, radio ms per hour) so power can
   be tracked without a meter, and log it once an hour.

Targets to agree before measuring: idle average under 1 mA with no notes waiting
(to be checked with a USB-C meter or INA219; the firmware will also report its own
duty cycle).

## 5. What happens to Hermes

- Device: the Hermes WebSocket protocol goes away. Firmware updates over USB
  (`pio run -t upload`) and, if wanted later, a small HTTP manifest pull.
- Host: unchanged. The gateway plugin keeps the notes endpoint and the
  transcription pipeline; the phone still posts to the container we deployed.
- Optional later: give the container its own OpenAI-compatible STT shim so the
  note path stops depending on the gateway. Not in this plan.

## 6. Documentation

- `README.md`: four steps become flash, Wi-Fi, phone (iOS **and** Android), notes.
  Live talk, pairing with Hermes and speech setup come out.
- `docs/using-gadget.md`, `docs/features.md`, `docs/protocol.md`,
  `docs/waveshare-epaper-154-v2.md` §6/§7: key map, no live, the new menu.
- New `docs/android-bluetooth-sync.md`: Chrome for Android, Nearby devices
  permission, the same page over the tailnet, OS pairing dialog, Fetch.
- `docs/ios-bluetooth-sync.md`: unchanged except the menu path and the removal
  of the Hermes pairing reason.
- CHANGELOG entry, one per phase.

## 7. Order of work, with a stop after each

0. **Baseline** (half a day): meter and log today's idle and per-upload cost, so
   the diet has a real before/after. Flash the current firmware and record duty
   cycle.
1. **Keys and power** (1 day): PWR = graceful power off, BOOT = tap record, hold
   menu. No live entry points left. Ships on the current firmware, testable today.
2. **Menu + notes browser** (2 days): list, play, delete with confirm, Sync now.
3. **The diet** (3-4 days): the notes app shell, module-by-module, behind
   `HG_APP=notes`; the old firmware stays buildable until the new one passes the
   same bench checks. Then delete the old target.
4. **Docs and release** (1 day): README, Android, spec, CHANGELOG; tag.

Bench checks that must pass before the old target is deleted: record and upload
over Wi-Fi; phone fetch and upload over BLE; delete and retention; power off from
battery and from USB; Wi-Fi setup from a phone; pairing ask; clock after SNTP;
flash and RAM numbers recorded in the spec.

## 8. Decisions (agreed 2026-11-21)

1. **Architecture B**: a dedicated notes app, reusing the proven modules.
2. **PWR tap powers off immediately**; the 2.5 s hold stays the hardware latch.
   No confirmation screen.
3. **Firmware updates over Wi-Fi**: a small HTTP manifest pull replaces the
   `ota.*` WebSocket messages; USB flashing still works.
4. **The BLE phone console service (`…0001`) is dropped.** The device advertises
   and serves the note service only.
5. **Menu selection is PWR**, power only outside the menu. BOOT stays the
   next-item key, and the delete confirmation is BOOT-hold = delete,
   BOOT-tap = keep.

Still open, to settle in phase 2: delete removes the card's copy only (the vault
note stays). Confirmation copy reads "Hold BOOT to delete, tap to keep."

## 9. Item 6: the phone site and the Docker app (nothing secret to type)

### What is true today

- The relay page asks for four fields: notes endpoint, device id, device key and
  (if set) an access token. All four are kept in `localStorage`, including the
  device key, because **the page signs the upload itself**: `X-Gadget-Mac` is
  `HMAC-SHA256(device_key, canonical)`.
- The device id is not readable over Bluetooth: `info` answers
  `ok ble-note-sync/1 mtu N` and nothing else.
- Discovery is the browser's chooser, filtered on the note service. One tap, but
  only after the fields are typed.
- The pairing PIN is already the security boundary: a phone must be allowed on
  the device (BOOT) and complete LE Secure Connections with a six-digit passkey
  before it can read a single note. The site itself has no login, and needs none.

### Design

1. **The device signs, the phone relays.** Add one control command,
   `sign <note_id>`, answered `ok {"ts":N,"mac":"base64"}` computed on the device
   with its own key (`hg::crypto` already does this for the Wi-Fi uploader). The
   page sends exactly those values as `X-Gadget-Ts` / `X-Gadget-Mac`. The server
   contract does not change, and **the key never reaches the phone**.
2. **The device id comes over Bluetooth.** `info` becomes
   `ok ble-note-sync/2 device hg-… mtu N`, so the page fills the id in itself.
   The id is public data (it is printed by `hermes gadget devices`).
3. **The endpoint is the page's own origin**, as it already is when the page is
   served by the container. Nothing to type.
4. **Remember the device.** `navigator.bluetooth.getDevices()` where the browser
   supports it (Chrome for Android does) reuses the granted device without a
   chooser; Bluefy may not, and the fallback is the one-tap chooser.
5. **Sign at send, not at fetch.** The server allows a 300 s old timestamp, so
   the page asks for a signature immediately before each POST attempt and asks
   again on retry. A note fetched today and posted tomorrow still works because
   the signature is fresh.
6. **Stop storing secrets.** On load the page deletes `relay.key` and
   `relay.token`; it stores only the endpoint and the device id.

### Why not a PIN on the site

Reading a note requires a paired phone (BOOT allow plus passkey), and uploading
it requires a signature only the device can produce. A site PIN would be a third
secret to type that protects nothing the first two do not.

### What the container keeps doing

`POST /notes/v1` verifies the same canonical string with the key it already holds
in `devices.json`, so the Docker app needs no change beyond logging the rejection
reason. It still exposes `/health`, still serves the page at `/`, and still writes
through the Hermes pipeline.

### Edge cases to handle

- Device clock unset (no SNTP yet): `sign` refuses with `no clock`, and the page
  says so instead of posting a signature the server will reject.
- A signature requested, then the phone sits on the page for ten minutes: post
  fails with 401, the page re-signs and retries once.
- Two notes fetched, one posted: signatures are per note (idempotency key and
  audio hash are in the canonical string).
- `getDevices()` granted but the device is asleep or out of range: fall back to
  the chooser, do not dead-end.
