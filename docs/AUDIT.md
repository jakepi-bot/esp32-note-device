# Audit: ESP32 note device firmware

Snapshot taken 2026-11-20 from the firmware as flashed on the physical board
(firmware version 0.6.0, build `d6300f7`), read against
`firmware/esp32/` and `firmware/core/` at the commit this file was added with.
Everything marked **verified** was read in source or observed on hardware;
everything else is called out as assumed or unmeasured. Re-check the source
before trusting a line here: the code is authoritative, this file is a map.

## 1. What the device is

A single-purpose handheld: an ESP32-S3 e-paper board running the Hermes Gadget
firmware, ported as board target `waveshare-epaper-154`
(`CONFIG_HG_BOARD_WS_EPAPER_154`, `main/board.cpp`), with an offline voice-note
stack added on top of the SDK's live assistant.

Two jobs in one firmware:

1. **Live assistant.** Hold BOOT → record → stream to Hermes over the gadget
   WebSocket → reply text on screen, reply audio through the speaker.
2. **Offline notes.** Tap PWR → record to the microSD card with no network →
   the note joins a durable queue → uploads itself to the notes endpoint when
   the device is next online and awake.

Both were exercised end to end on the board on 2026-11-20 over Wi-Fi (3 live
turns, note recorded offline and uploaded, note visible on the Hermes side).

## 2. Hardware

Source of truth for the firmware is `BoardConfig make()` for the board, plus
`docs/hardware.md` and the vendor demo/schematic (`docs/waveshare-epaper-154-v2.md`
§2 records which was read).

| Item | Value | Confidence |
|---|---|---|
| Module | ESP32-S3-PICO-1-N8R8: 8 MB flash, 8 MB octal PSRAM, dual-core 240 MHz | verified in `boards/waveshare-epaper-154/sdkconfig.defaults` and at run time (`diag`) |
| Radio | Wi-Fi 2.4 GHz (used); BLE 5 capable (not enabled — see §7) | verified |
| Display | 1.54" 200×200 1-bit e-paper, SSD1681-class controller, 4-wire SPI, partial refresh | controller family inferred from the LUT/command set; not read from a datasheet |
| Audio | ES8311 (DAC and ADC), NS4150B amp, 8 Ω speaker header, analog mic | codec verified at run time (I2C 0x18; speaker and mic both ready) |
| Storage | microSD, 1-bit SDIO, FAT32 | verified (card mounts, notes persist) |
| Buttons | BOOT (GPIO 0) = TALK, PWR (GPIO 18) = note key / power | verified |
| Power | USB-C, ETA6098 charger, single-cell Li-ion, GPIO 17 latch, GPIO 4 battery sense through a 2:1 divider | divider and latch verified in code; charger part from schematic, unverified in firmware |
| Not used by firmware | SHTC3 temp/humidity (0x70), PCF85063 RTC (0x51), green LED | present on the board per the vendor docs; no driver is bound |

**Pinout (authoritative, `main/board.cpp`)**

| Function | GPIO |
|---|---|
| EPD SCLK / MOSI / CS / DC / RST / BUSY | 12 / 13 / 11 / 10 / 9 / 8 |
| EPD supply rail (active low, opened before the first transfer) | 6 |
| I2C SDA / SCL (400 kHz) | 47 / 48 |
| I2S MCLK / BCLK / WS / DOUT / DIN | 14 / 15 / 38 / 45 / 16 |
| Speaker amp enable | 46 |
| Audio rail (active low, 50 ms settle before the codec starts) | 42 |
| SD CLK / CMD / D0 | 39 / 41 / 40 |
| Battery ADC (2:1) / battery latch (high = stay on) / PWR key | 4 / 17 / 18 |
| BOOT (TALK) | 0 |

Assumptions worth stating:

- **Non-touch SKU.** The Touch variants of this board carry an FT6336; the board
  config has no touch controller and no backlight. All screen input is the two
  buttons.
- **No RTC in use.** The board may carry a PCF85063, but nothing binds it, so
  "time" means SNTP only (see §6). That is why notes recorded before the first
  sync get restamped at upload.
- **Battery level is a raw ADC voltage**, with a 2:1 divider and
  `usb_external_power = true`. Charge *state* (charging / full) is not read from
  any GPIO today, so a "charging" indicator has no driver behind it yet.
- Full and partial refresh durations, and the BUSY polarity, are taken from the
  vendor demo, not measured with an instrument.

## 3. Display stack

- **No vendor graphics library.** `main/port_display_epd.cpp` implements the
  panel directly over ESP-IDF's `spi_master`: power-on sequence, full and partial
  159-byte waveform LUTs (153 bytes for command `0x32` plus the five trailing
  bytes), RAM writes, BUSY polling. LUT and register data are the only
  vendor-derived material and are cited to the Waveshare demo commit in the file
  header.
- **Framebuffer**: the core's normal RGB565 framebuffer (200×200×2 = 80 KB) lives
  in PSRAM; `flush(y0,y1)` converts only the changed rows to 1 bit and marks the
  panel dirty. A helper task coalesces the bands of a frame and runs the refresh
  off the app task, because a BUSY wait outlasts the frame budget.
- **Refresh policy**: partial refresh normally, forced full refresh on redraw
  requests or every 20th partial (`kFullEvery = 20`), which is the visible flash.
- **Inspection without a camera**: `screen` on the console prints the panel image
  as hex, which is how the display was verified during bring-up. Any display
  work should keep this working.
- **No backlight**, so brightness and screen-timeout code paths are no-ops on
  this board; `set_backlight` and the PWR screen toggle do nothing.
- **Text rendering** comes from the core's UI (`firmware/core`), same face/status
  model as the round screens, scaled into 200×200. The e-paper build uses the
  text-first paths; there is no animation by default.

## 4. Audio stack

- One duplex I2S bus: ES8311 DAC → NS4150B → speaker, ES8311 ADC → mic.
  `main/port_codec.cpp`, `main/port_audio.cpp`.
- Rate is fixed at **24 kHz PCM16 mono** (`CodecAudio::kRate`); `start()` refuses
  other rates. This is also the note format, so no resampling happens anywhere
  between mic and upload.
- Notes are written as WAV by `notes/notes_capture.cpp`; live turns are streamed
  to Hermes in the gadget protocol's own framing.

## 5. Build toolchain

- **ESP-IDF 6.1** (CI container `espressif/idf:v6.1`); the CMake header says
  "ESP-IDF 5.3+; built with 6.1".
- **PlatformIO 6.2.0** wraps it for the S3 boards: env `waveshare-epaper-154`
  uses `board = esp32-s3-devkitc-1` with 8 MB flash, and passes
  `-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-epaper-154/sdkconfig.defaults"`.
- Config is stacked defaults, and `tools/check_config.py` fails the configure
  step when a defaults line did not reach the generated `sdkconfig` — a typo or a
  stale `sdkconfig` cannot silently build the wrong board.
- `managed_components/`, `dependencies.lock` and `sdkconfig` are generated and
  gitignored; the first build fetches them.
- Flash layout: custom `partitions.csv` with two ~1.9 MB app slots and a
  `coredump` partition; OTA rollback is armed (a new image must reach Hermes
  within 5 minutes or the bootloader reverts).
- Release packaging: `tools/package_release.py` turns a build into the image
  bundle CI uploads.
- Build time from a clean checkout: see §10.

## 6. Clock and time

- Time comes from **SNTP** (`main/port_time.cpp`). The board has no RTC bound, so
  before the first sync `std::time()` reads 1970.
- The note id embeds the recording time, so a note recorded pre-sync is
  *undated*: its id says 1970 and it keeps that id (it doubles as the idempotency
  key). At upload a note whose stamp is implausible (older than
  `kMinTrustedStampUnix`, 2024-01-01) is sent with the current time instead,
  flagged `restamped`, and the local `note.json` keeps its original bytes so
  retries stay byte-identical.
- `notes` on the console counts how many pending notes are still undated.
- Recent fix (2026-11-20): the clock is requested whenever SNTP has not yet
  answered, not only when the resting clock face is enabled — otherwise a device
  with the clock face off never synced and all notes stayed undated.

## 7. Bluetooth: what exists, what does not

- There is a **BLE phone-link implementation**: `main/port_ble.cpp` (NimBLE) with
  `PhoneLink` in `port.hpp` — advertising, pairing, bonds, and a GATT channel
  that carries **console command lines and `@` replies** to a phone app.
- It is **compiled out**: the whole file is behind `#if CONFIG_BT_NIMBLE_ENABLED`,
  and no board's sdkconfig defaults enable Bluetooth (`docs/protocol.md` states
  "none yet"). The e-paper build has no Bluetooth stack at all today.
- So a Bluetooth note-sync feature starts from a working console bridge but has
  to (a) enable NimBLE for this board, (b) add a transfer path — the console
  bridge can already run `notes list` / `notes sync`, but audio bytes need a
  dedicated characteristic or a chunked command — and (c) account for the flash
  and RAM the stack costs in a two-slot OTA layout.

## 8. Note data model

Defined in `firmware/esp32/main/notes/` and specified in
`docs/waveshare-epaper-154-v2.md` §7 and `docs/voice-note-ingestion.md`.

**Card layout** (root `/sdcard/notes`):

```
pending/<note_id>/{recording.wav,note.json}
sent/<note_id>/...
failed/<note_id>/...
```

`note_id` = `<device_id>-<YYYYMMDDTHHMMSS>-<seq:04u>`, time-sortable, and equal
to the idempotency key.

**`note.json` fields** (`notes::Meta`):

| Field | Meaning |
|---|---|
| `note_id` | directory name; idempotency key |
| `device_id`, `device_name` | the paired gadget identity |
| `firmware` | firmware version string |
| `created_ms` | unix ms, RTC/SNTP-backed |
| `recorded_at` | ISO-8601 UTC, `YYYY-MM-DDTHH:MM:SSZ` |
| `duration_s` | 1..900 (device caps at `note_max_s`, default 300) |
| `idempotency_key` | equal to `note_id` |
| `bytes` | size of `recording.wav` |
| `sha256` | lowercase hex of the WAV, filled before the first upload |
| `attempts` | bumped by the uploader, informational |
| `battery_mv` | 0 = unknown |

**Durability rules** (these are the contract, not implementation detail):

- A note exists only once its `note.json` is renamed into place; audio is written
  as `recording.wav.part` and renamed *before* the metadata.
- Deletion removes audio before `note.json`, so a power cut leaves at worst an
  empty folder.
- Failed uploads keep the note in `pending/` and retry on a backoff; server
  rejections move it to `failed/`, which is terminal and only emptied by
  `notes delete --failed`.

**Limits**: audio ≤ 32 MiB, duration 1..900 s, sample rate fixed at 24 kHz.

## 9. Note upload path

- `POST http://<hub-host>:8766/notes/v1`, multipart/form-data, HMAC-protected.
  Default port 8766; `extra` value `notes_url` overrides the whole URL.
- Served by the Hermes gadget plugin inside the gateway process
  (`plugin/notes_ingest.py`, pipeline in `plugin/notes_pipeline.py`); `GET
  /health` reports `ok|paused`, port, inbox/done/failed counts.
- The uploader is sliced across app-loop passes so a POST never stalls the loop;
  it only runs when the device is online and the clock is set.
- Plain HTTP on LAN/tailnet: the HMAC authenticates, it does not encrypt.

## 10. Verification performed for this audit

- Source read: `main/board.cpp` (board entry), `main/port_display_epd.*`,
  `main/modes/notes_mode.*`, `main/notes/*`, `main/port_ble.cpp`,
  `main/CMakeLists.txt`, `platformio.ini`, `sdkconfig.defaults`,
  `boards/waveshare-epaper-154/sdkconfig.defaults`, `docs/hardware.md`,
  `docs/voice-note-ingestion.md`, `docs/protocol.md`.
- Clean-checkout build from the pushed repository (fresh clone, documented
  PlatformIO steps): see the build log in the "Clean build" section below.
- On-hardware evidence for the behaviours in §1 is the flash-and-validate run of
  2026-11-20 (firmware 0.6.0): Wi-Fi join, pairing, 3 live turns, one offline
  note recorded and uploaded, note visible to Hermes.

### Clean build

See [BUILD-VERIFICATION.md](BUILD-VERIFICATION.md) for the recorded command,
toolchain versions, image size and result of building this repository from a
fresh clone.

## 11. Gaps and risks for planned work

| Area | State | Risk for the next tasks |
|---|---|---|
| Status display (battery, time, Wi-Fi, BT) | Today the screen shows the core's status/face UI and the notes mode's single status line (`n pending, m sent`, `No notes`, `No SD card`). No battery or clock widget is drawn. | Battery needs the GPIO 4 ADC path and a charge-state source that does not exist yet; BT icon has nothing to report until BLE is enabled; e-paper must not be repainted more often than the refresh policy allows, so "live" indicators need a sensible cadence and partial-refresh budgeting. |
| Bluetooth note sync | Console bridge exists but compiled out. | Enabling NimBLE changes flash size (two OTA slots), RAM, and the pairing UX; there is no phone-side app today, and Conduit for iOS is not a BLE data client — a Shortcuts/Conduit bridge or an in-app BLE path needs deciding. **The app already uses 88.9% of its OTA slot (11% / ~180 KB free, see [BUILD-VERIFICATION.md](BUILD-VERIFICATION.md)); the BLE stack is unlikely to fit without growing `partitions.csv` or trimming features.** |
| Note metadata on BLE | Notes are files on FAT; no manifest is exposed over any local channel. | A transfer path needs a manifest (id, duration, size, sha256) and a chunked read, plus resume handling. |
| Time without Wi-Fi | SNTP only; no RTC bound. | If BLE sync is meant to work away from Wi-Fi, binding the board's PCF85063 (if the SKU has it) would give real stamps offline. |
| Docs drift | `docs/hardware.md` still says the e-paper port "has not been run on one". | It has now been run and validated; either correct that line or leave a pointer to this audit. |
| Colour/graphics | 1-bit panel, no greyscale waveform in use. | Any "readable" redesign has to work in pure black/white at 200×200; text sizes are the lever, not colour. |
