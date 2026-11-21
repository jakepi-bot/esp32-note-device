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

## 10. The notes build, as it stands (2026-11-21)

`pio run -e notes` builds the notes-only app. It is the `waveshare-epaper-154`
board, partition table and sdkconfig; `-DHG_APP=notes` in `platformio.ini` picks
the other source list in `firmware/esp32/main/CMakeLists.txt`.

The table below is the **skeleton**, the first build of this target, kept because
it shows what the source split alone saved. It is not the shipped size. The app
that is on the board - runtime, screens and all - measures 1,464,175 B and 63,136 B
of static RAM, 36.0% of the slot against the Hermes app's 50.5%; the measurements
are in section 11 below and their section-by-section deltas are in the rewrite's
QA report.

| Skeleton, 2026-11-21 | Hermes app (`waveshare-epaper-154`) | Notes app (`notes`) | Difference |
|---|---|---|---|
| Image (`firmware.bin` / idf "Flash") | 2,050,799 B, 50.5% of the slot | 731,603 B, 18.0% | **-1,319,196 B (-1,288 KB, -64.3%)** |
| Static RAM (idf "RAM") | 87,836 B, 26.8% of 320 KB | 35,440 B, 10.8% | **-52,396 B (-59.7%)** |
| `ota_0` slot free (`tools/check_size.py`) | 50% | 82% | |

Both numbers are from a clean `pio run` on 2026-11-21 with the same ESP-IDF
(6.1.0) and toolchain. The skeleton was small because nothing referenced the notes
path: with no uploader, station or setup page in the image, `--gc-sections`
dropped them. Wiring the runtime in is what links the HTTP client, the Wi-Fi
station/AP stack and the TLS bundle, and that is most of the 731,603 B to
1,464,175 B.

Against the Hermes app the shipped notes image is **574 KiB smaller in flash**
(2,052,223 B to 1,464,175 B, 50.5% to 36.0% of the slot) and **24.2 KiB smaller
in static RAM** (87,948 B to 63,136 B). The plan's guess was 250-350 KB of flash
and 40-80 KB of RAM; flash came in above the band, RAM below it, because the
core's bulk is flash rodata (mascots, the i18n table, panel init tables, the
BMI270 blob) rather than DRAM.

### Where the two apps live

- `firmware/esp32/notes_app/main.cpp`: the notes app's entry point. It brings up
  the board (rails, battery latch, panel, codec), mounts the card, starts the
  radio, starts the console and then rests. The panel shows a frameless boot
  image it fills itself, so no font or canvas is needed to prove it; the console
  answers `screen`, `sd`, `keys`, `menu` and `about`; the keys are answered by
  `ui_keys.*` and logged.
- `firmware/esp32/main/notes_app/ui_keys.*`: unchanged, still the screen and key
  rules, still with its eight host cases in `firmware/tests/test_notes_ui.cpp`
  (part of `hg_epaper_mode_tests`, 42 cases).
- `firmware/esp32/main/CMakeLists.txt`: `HG_APP` (`hermes` default, `notes`)
  selects one of two source lists over a shared base. The shared base is the
  board table, the notes stack and the ports the product keeps; the Hermes list
  adds `main.cpp`, the mode modules and every board's panel driver.

Two seams were needed, and both are named where they sit:

- `port_diag.cpp`'s touch statistics are behind `#if !HG_NOTES_APP`, because the
  notes app keeps no `port_touch.cpp` and nothing sets `touch_input` there.
- `port_audio.cpp` is in the shared base: `port_codec.cpp` calls
  `hgp::speaker_now_ms()`, which lives beside the I2S speaker that shares its
  clock. The I2S classes it also defines are unreferenced and dropped.

### Nothing from the dropped modules is in the notes image

`xtensa-esp32s3-elf-nm -C --defined-only` on both `.elf` files, count of
demangled symbols matching:

| Pattern | Notes app | Hermes app |
|---|---|---|
| `hg::App` | 0 | 285 |
| `hg::UiModel` | 0 | 77 |
| `hg::Canvas` | 14 | 42 |
| `i18n` | 0 | 25 |
| mascot / face data | 0 | 37 |
| `opus` | 0 | 20 |
| `WebSocket` / client | 0 | 17 |
| tailnet / tailtiny | 0 | 14 |
| camera | 0 | 8 |
| `TouchInput` | 0 | 24 |
| protocol JSON | 0 | 4 |
| font tables | 0 | 11 |

`hg::Canvas` is the one row that is not zero, and it is deliberate: the notes
renderer draws its own screens on the SDK's 1-bit canvas rather than on a
framebuffer of its own, so the 14 symbols are the canvas it draws through, not the
core UI that used to drive it.

What the notes image does carry from the core: `hg::json`, `hg::RadioWindow`,
`hg::LogRing`, `hg::SharedReply`, `hg::SpeakerPa`, `hg::Canvas`, the display base
class and the two RTC drivers. NimBLE is there, because the phone path is.

### Built on top of this skeleton

All three landed: the nine screens ([the spec](notes-ui-spec.md),
[as built](notes-ui-implementation.md)), the power and radio schedule (section
11 below), and the documentation pass this page is part of. The Hermes app's target stays
buildable until the notes app has passed the bench checks in section 7 on the same
unit, which is why `-e waveshare-epaper-154` is still in `platformio.ini`.

## 11. The runtime: power, the radio's duty cycle, Wi-Fi/SNTP (2026-11-21)

`firmware/esp32/notes_app/runtime.cpp` is everything the notes device does that
is not a screen, written against `hg::App`-free modules rather than
`modes/notes_mode.cpp` (which needs the core's App, UI state and settings):

| Piece | Where it comes from |
|---|---|
| the card tree, the recording, the queue and its HMAC | `notes/notes_store`, `notes_capture`, `capture_writer`, `notes_queue`, `notes_uploader` |
| the phone's transfer | `notes/notes_ble` over `port_ble.cpp`'s GATT table, pairing ask and `hg::RadioWindow` |
| the retention rule | `notes/notes_retention`, on the same 5-minute / 1-minute-low clock |
| the battery | `port_latch_power.cpp` (ADC, `hg::percent_on_curve`, or a 3.30-4.15 V line on this board, which has no measured curve) |
| the station and the setup page | `port_wifi.cpp`, `port_wifi_setup.cpp` |
| the clock | `port_time.cpp` (SNTP; this board has no RTC) |

Two seams were needed in the shared ports, both to stop them needing `hg::App`:

- `Wifi::connected()`/`tick(now)` take no app. What they used from it is now a
  set of hooks the app sets: `on_console` (the setup page's `set server …`),
  `open_setup`/`close_setup` (the app's screen; empty means the port starts and
  stops the page itself), `in_turn`, `dozing`, `deep_sleep_ok` and `traffic_at`.
  The Hermes app wires them to `hg::App` in `main.cpp` and behaves as before.
- `Wifi::radio_off()`/`radio_on()` are new: the station is **stopped** outside a
  window (an unassociated station still costs idle current and holds the chip
  out of light sleep), and started plus rejoined when one opens. `setup_open()`
  keeps a phone's setup page from being closed by a window's deadline.

`port_wifi_setup.cpp`'s tailnet block is behind `#if !HG_NOTES_APP`: the notes
app has no tailnet (section 2), and asking `TailnetLink` there would link
`port_tailnet.cpp` into an image that carries no TailTiny.

### The duty cycle

| Event | What the radio does |
|---|---|
| boot with a pending note or an unset clock | Wi-Fi window, 120 s (SNTP runs as soon as the station has an address) |
| a note finishes recording | BLE window (`RadioWindow`, 180 s) and a Wi-Fi window, 120 s |
| `Sync now` (menu or console) | a window at once, 120 s |
| phone setup page | the station stays up for the page's own lifetime |
| window closed with work left | reopened every 15 min (2 min up, 13 min down) until the queue is empty and the clock is set |
| at rest | no association, station stopped; the BLE controller is off outside its window |

`notes status` reports the Wi-Fi window's share (`radio <up>/<since-boot> ms`)
so the duty cycle is measurable without a meter; the target is still the one in
section 4 (idle average under 1 mA), to be measured on the bench.

### Power

PWR is the graceful path: a 1.5 s hold (half a second before the latch's own
cut) runs `Runtime::shutdown()`, which finishes a recording as a note (its
`note.json` is the commit), drains the writer, cancels an upload (the note stays
`pending/` and is resent by key), releases the card, closes the station and drops
the latch. On USB the latch is not the supply, so the card is mounted again and
the device carries on instead of sitting dead with no card.

### Numbers, measured

`pio run -e notes` on this tree, clean build, ESP-IDF 6.1.0, 2026-11-21:

| | Hermes app (`waveshare-epaper-154`) | Notes app with the runtime (and the screens card's files) |
|---|---|---|
| Image | 2,050,799 B | 1,460,155 B |
| Static RAM | 87,836 B | 63,136 B |

The parent's skeleton was 731,603 B only because nothing referenced the notes
path: `nm` found no `hg::App`, but the uploader, the station and the setup page
were unreachable, so `--gc-sections` dropped them. Wiring the runtime in is what
links `esp_http_client` (via `notes_uploader.cpp`), the Wi-Fi station/AP stack
(`esp_wifi`, net80211, wpa_supplicant, the mesh objects), `esp_http_server` and
the mbedTLS X.509 bundle; that is most of the difference, and it is the price of
the upload, the setup page and SNTP. The notes image is still 577 KB (28%)
smaller than the Hermes app in flash and 28% smaller in static RAM.

### On the desk unit (2026-11-21)

Flashed `pio run -e notes -t upload` over `/dev/ttyACM0` and driven from the
host over the console (`about`, `battery`, `clock`, `radio`, `wifi`, `keys`,
`notes …`, `phone …`, `screen`), one pass per acceptance item:

| Item | Evidence |
|---|---|
| boots to the status face | `hg.ui: screen home, 57 notes (0 to send)` and a full refresh at 3.7 s; `screen` read back over the console decodes to Home: `ALL SENT`, `READY`, the key hint |
| joins the configured Wi-Fi | `hg.wifi: connected, ip 192.168.1.106` inside the window; `about` answers `wifi on` only then |
| SNTP sets the clock | `hg.time: asking pool.ntp.org for the time` at the address, `hg.notes: clock synced` 1.3 s later, `SNTP stopped (the network went down)` when the window closes |
| uploads a pending note with a valid HMAC | `hg.notes.up: note …: HTTP 202 -> accepted` and the gateway's `notes_ingest` line for it; a 202 is only sent after `protocol.verify_note_mac` passes against the enrolled device key |
| BLE pairing ask, phone client | `hg.phone: a phone asks to pair; waiting for the screen` on connect, allow, `pairing passkey 449047` shown on the device and typed by the client, `bluetoothctl` then reports `Paired: yes` (bonded, passkey entry) |
| battery percentage sane | `100% (4164 mV)` on USB; the board has no charge-signal pin, which the console now says instead of "discharging" |
| clean shutdown, no lost writes | `notes poweroff`: recording finished, writer drained, card unmounted, latch left up because the board is on USB, card remounted, `58 notes`, `card ok`, `sd 62.9 GB free` — the count is unchanged by the pass |
| the phone's setup AP and page | `wifi-setup` on the console: `Network: Hermes-75CA`, a WPA2 password, `Open http://192.168.4.1`; a second adapter joined, the AP's DHCP server assigned `192.168.4.2`, `GET /` answered 200 with the 5,482-byte page and `/api/setup` answered its `nonce`/`state`/`ssid` JSON; `wifi-setup stop` closed the page and left the station stopped |

Three defects the desk pass found, all fixed here:

- **The station's link was never reported.** `Wifi::connected()`/`disconnected()`
  were called by `hg::App` on `NetUp`/`WifiDisconnected`; the notes app has no
  `hg::App` and dispatched neither, so `current()` stayed empty after a real join.
  The window then uploaded nothing and never started SNTP, and the retry clock
  kept calling `esp_wifi_connect()` on a connected station ("disconnect before
  connecting to new ap"). `Wifi::tick()` now reads the link from the driver
  (`esp_wifi_sta_get_ap_info` plus the station's address) and applies the
  transition; both transition functions are idempotent, so the Hermes app's event
  path is not competing (`loss_applied_` keeps the backoff from being counted
  twice).
- **The console never saw the notes app's own commands.** `about`, `battery`,
  `clock`, `radio`, `sd` and `keys` answered "Unrecognized command": `esp_console`
  answers before the app is asked, and only `notes`/`screen` were registered.
  They are now registered behind `#if HG_NOTES_APP`.
- **Dropping the latch on USB killed the card instead of the device.** The card
  sits on the latched rail, so `power_off()` (which only writes the pin) left a
  running board with no storage. `Runtime::shutdown()` now leaves the latch alone
  when external power is present, reports it, and remounts: a person unplugs it.

Three console verbs exist for this bench work and for QA without a finger on the
board: `notes poweroff` (the PWR-hold path), `phone allow` / `phone refuse` (the
two answers the pairing ask takes from a key), and `wifi-setup` / `wifi-setup
stop` (the menu's row that opens the phone's setup page).

Measured duty cycle in the pass: radio up 26.5 s of 69.3 s of uptime across two
windows, and the counter did not move afterwards (`26.457/74.147 s` at rest) —
the device holds no association between windows. The first join attempt after a
boot ends in `reason 2 auth expired` and succeeds on the port's 20 s retry; that
is the existing fast-join fallback, not the window logic, and it costs about
20 s of a 120 s window.
