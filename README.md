# ESP32 Note Device (Jakebot)

Firmware for a pocket voice-note and assistant device built on the **Waveshare
ESP32-S3-ePaper-1.54 (V2)**: press PWR to talk to Hermes, tap BOOT to record an
offline voice note to the microSD card, and the panel goes back to its home
screen after a minute with no input, and let the device upload the queue to
Hermes whenever it is next online.

This repository is the canonical home of that firmware. It is a focused fork of
the [Hermes Gadget SDK](https://github.com/Adolanium/hermes-gadget-sdk) (MIT) and
keeps the upstream tree whole on purpose: every board target, the shared device
core, the host tests, the Hermes plugin, and the documentation still build here,
so upstream fixes can be merged instead of re-ported. The target board to build
and flash is `waveshare-epaper-154`.

Status (2026-11-20): **release 0.7.1**. The firmware builds from a clean checkout,
is flashed on a physical board, and has been validated end to end over Wi-Fi —
pairing to a live Hermes hub, live assistant turns (record → stream → reply →
spoken reply), and offline voice notes recorded to the card and uploaded to the
notes endpoint. 0.7.1 fixes the three defects the 0.7.0 hardware QA recorded as
known limitations (`notes stop` during a panel refresh, the pairing screen's
inverted polarity, the newest-note line before a first clock sync); 0.7.0 added the redesigned [e-paper status screen](#display-what-the-panel-shows)
and [Bluetooth note sync](#bluetooth-note-sync-from-an-iphone).
Bluetooth now initialises on the board — the GATT table registers, the `phone` and
`notes ble` commands answer, and pairing opens advertising — but no phone has
completed a transfer yet, so that half is unverified: see
[docs/ble-note-sync.md §11](docs/ble-note-sync.md#11-what-has-been-verified-and-what-has-not)
and the [hardware QA report](docs/qa-report-0588422.md).
Hardware detail and assumptions are in [docs/audit.md](docs/audit.md).

## Install a release

Prebuilt images are on the [releases page](https://github.com/jakepi-bot/esp32-note-device/releases/latest);
the README's build steps are for changing the firmware, not for running it.

| File | Use it for |
|---|---|
| `hermes-gadget-waveshare-epaper-154-0.7.1.bin` | the whole flash image, from `0x0`: a fresh board, or the one USB flash the Bluetooth build needs |
| `hermes-gadget-waveshare-epaper-154-0.7.1-app.bin` | `hermes gadget update <device> <file>`, an over-the-air update of a board already on 0.7.x's partition layout |
| `SHA256SUMS`, `manifest.json` | checksums; what the browser installer reads |

```bash
esptool.py --chip esp32s3 write_flash 0x0 hermes-gadget-waveshare-epaper-154-0.7.1.bin
```

Writing the full image erases the settings area (NVS), so a device flashed this way
pairs again as a new device. Keep its Wi-Fi password and its key at hand. The
browser installer at <https://jakepi-bot.github.io/esp32-note-device/> does the same
over USB from Chrome or Edge and leaves the settings alone.

Bluetooth does not fit the shared 1.9 MB app slots, so this board builds
`partitions-epaper-8mb.csv` (two 3,875 KB slots). A board coming from 0.6.0 needs
**one USB flash** of the full image before over-the-air updates work again — an OTA
cannot move partitions. `nvs` keeps its offset, so settings and paired phones
survive it.

## Hardware

| Part | Detail |
|---|---|
| SoC | ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB octal PSRAM, Wi-Fi 2.4 GHz, BLE 5 capable (Bluetooth is on: phone link + note sync) |
| Display | 1.54" 200×200 1-bit e-paper, SSD1681-class controller, SPI, partial refresh |
| Audio | ES8311 codec (DAC + ADC on one I2S bus) → NS4150B amplifier → 8 Ω speaker, analog mic |
| Storage | microSD, 1-bit SDIO, FAT32 |
| Buttons | PWR (GPIO 18) = talk to Hermes (a short press starts or ends a turn; a hold is the hardware power-off); BOOT (GPIO 0) = voice note (a tap starts or stops one, a 2 s hold drops it) |
| Power | USB-C, ETA6098 charger, single-cell Li-ion, GPIO 17 battery latch, GPIO 4 battery sense (2:1 divider) |

### Pinout

| Function | GPIO |
|---|---|
| EPD SCLK / MOSI / CS / DC / RST / BUSY | 12 / 13 / 11 / 10 / 9 / 8 |
| EPD panel rail (active low) | 6 |
| I2C SDA / SCL (400 kHz) | 47 / 48 |
| I2S MCLK / BCLK / WS / DOUT / DIN | 14 / 15 / 38 / 45 / 16 |
| Speaker amplifier enable | 46 |
| Audio rail (active low) | 42 |
| SD CLK / CMD / D0 | 39 / 41 / 40 |
| Battery sense / latch / power key | 4 (ADC, 2:1) / 17 / 18 |

The full table with notes is in [docs/hardware.md](docs/hardware.md#waveshare-esp32-s3-epaper-154)
and the board's config lives in `firmware/esp32/main/board.cpp`.

## Build

Two toolchains work; PlatformIO is the shortest path and is what CI uses.

### PlatformIO (recommended)

```bash
pip install platformio==6.2.0     # 6.2.x is the version CI pins
cd firmware/esp32
pio run -e waveshare-epaper-154            # build
pio run -e waveshare-epaper-154 -t upload  # flash over USB-C
pio run -e waveshare-epaper-154 -t monitor # serial console, 115200 baud
```

PlatformIO fetches ESP-IDF 6.1 and the managed components on the first build
(`managed_components/` is generated, not committed) into `~/.platformio`.

### ESP-IDF 6.1

```bash
. $IDF_PATH/export.sh
cd firmware/esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-epaper-154/sdkconfig.defaults" \
       -D SDKCONFIG="sdkconfig.epaper" set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-epaper-154/sdkconfig.defaults" \
       -D SDKCONFIG="sdkconfig.epaper" build
idf.py -p /dev/ttyACM0 flash monitor
```

The USB-C port is the S3's own USB Serial/JTAG, so flashing and the serial
console share it (115200 baud). Older ESP-IDF releases build but are not what CI
and the hardware validation used.

## First boot and pairing

1. Flash, then watch the console: expect `200x200 e-paper ready …`, `durable
   config ready …`, `codecs: speaker ready, microphones ready`, and `display:
   ssd1681` in `diag`.
2. Join Wi-Fi: the board raises its setup access point when it has no
   credentials; the `wifi` console command also takes a network and password.
3. Point it at Hermes and pair it: `hermes gadget pair`, then approve the code
   shown on the device. See [docs/connect-hermes.md](docs/connect-hermes.md).
4. The notes endpoint is derived from the hub's host on port 8766. Override it
   with `set notes_url http://<host>:<port>/notes/v1` if the hub sits behind a
   proxy or another port.
5. Set the two defaults an out-of-the-box screen gets wrong. Both are off/unset
   by design, so nothing guesses at hardware that may not be fitted:

   ```sh
   set timezone "CST6CDT,M3.2.0,M11.1.0"   # POSIX TZ; unset shows UTC (default UTC0)
   set battery_estimate liion              # only with a single-cell Li-ion pack fitted
   ```

   Until `timezone` is set the clock is UTC, and until `battery_estimate` is set
   the footer reads `NO BATTERY` even with a pack fitted: `set battery_estimate
   liion` is exactly what turns that footer into a percentage once a single-cell
   pack is fitted (the board has no fuel gauge; the percentage is a voltage
   estimate). See
   [battery indicator settings](docs/using-gadget.md#device-settings-and-hardware-checks)
   and QA report [D2](docs/qa-report-0588422.md).

## Notes: what the device does

- **Tap BOOT** → record; **tap again** → stop and commit the note to the card;
  **hold BOOT 2 s** → drop the recording. No network is needed at any point.
- Each note is a folder `notes/<state>/<note_id>/` holding `recording.wav`
  (24 kHz, PCM16, mono) and `note.json` (metadata), where `<state>` is one of
  `pending`, `sent`, `failed`.
- The uploader drains `pending/` to `POST /notes/v1` (multipart) whenever the
  device is online and the clock is set, with exponential backoff and a durable
  queue, so a power cut loses nothing. `sent/` keeps the acknowledged notes,
  `failed/` keeps server rejections for inspection.
- Note ids are `<device_id>-<YYYYMMDDTHHMMSS>-<seq>`, which sorts by time and
  doubles as the idempotency key (retries are byte-identical).

- With no network at all, a phone can take a note over **Bluetooth LE** instead:
  a paired phone lists the pending notes, pulls one with resumable, CRC-checked
  blocks, posts it to Hermes, and only then does the device move it to `sent/`.
  The phone side is a Web Bluetooth page in `web/ble-note-relay/` (Chrome, Edge,
  or Bluefy on iOS). [How it works](docs/ble-note-sync.md).

Wire contract: [docs/voice-note-ingestion.md](docs/voice-note-ingestion.md).
Card layout and uploader design: [docs/waveshare-epaper-154-v2.md](docs/waveshare-epaper-154-v2.md) §7.
Bluetooth service, protocol and the phone workflow: [docs/ble-note-sync.md](docs/ble-note-sync.md).

Console commands (type `help` for everything else):

| Command | Effect |
|---|---|
| `notes` | counts, card state, target URL, mode, queued bytes |
| `notes record` / `notes stop` | record from the console |
| `notes sync` | force an upload pass |
| `notes list` | ids per state |
| `notes ble` | Bluetooth link, MTU, the note in flight, why the last one stopped |
| `notes delete <id>` / `notes delete --failed` | remove notes |
| `notes mount` / `notes format` | remount or format the card |
| `screen` | hex dump of the 1-bit panel image (proves what is shown) |
| `diag` | tasks, stacks, memory, i2c, display controller |

## Display: what the panel shows

The resting panel answers two questions at a glance: what is this device doing, and
can it do it. `main/modes/status_screen.cpp` is the approved design's reference
renderer, ported unchanged, so every state below is a frame that was signed off
rather than a look-alike:

- **Masthead.** The clock, then three icons: battery level (with a bolt for external
  power), Wi-Fi, Bluetooth.
- **Hero word.** One word that always carries information: `8 NOTES`, `RECORDING`,
  `OFFLINE`, `LOW POWER`, `NO TIME`, `NO CARD`, `ERROR`. There is no content-free
  `READY`.
- **Newest note.** Its time and length — `Newest note 01:47, 16 s` — or
  `Tap BOOT to add another` when the card is empty.
- **Footer.** The battery percentage plus the one fact that matters: `2 QUEUED`,
  `SYNCED`, `ON POWER`.

Black ink on paper, at 1.94 mm (scale 2) as the type floor and 2.91 mm (scale 3) for
the clock and the hero word; the core UI's older white-on-black at 0.97 mm is gone
from the resting screen. Handing the panel over from the core UI forces a full
waveform, so the two never ghost through each other, and a panel whose facts have
not changed is not refreshed at all. This board also ships with animation off
(`set animation on` turns it back on).

Two settings decide what the panel can say, both deliberately off out of the box
(see [First boot](#first-boot-and-pairing)): `timezone`, without which the clock and
every note time read UTC, and `battery_estimate liion`, without which the footer
says `NO BATTERY` even with a pack fitted — the board has no fuel gauge, so the
percentage is a voltage estimate and the firmware will not guess at a cell that may
not be there. The bolt means external power, never "charging": there is no charge
line on this board.

To check the panel without a camera, `python firmware/tools/panel_dump.py --port
/dev/ttyACM0 --out panel` walks the `screen <first> <count>` console command and
writes `panel.pbm` (1:1) plus `panel.txt` (ASCII). The frame hashes of ten states are
pinned in `hg_epaper_mode_tests`.

## Bluetooth note sync from an iPhone

Bluetooth is part of this board's firmware, not an optional extra: with no Wi-Fi
anywhere, a paired iPhone takes a note off the card over Bluetooth LE and posts it to
your server, which transcribes it and writes it into Obsidian. The device only moves a
note to `sent/` after the phone reports a 2xx, so an interrupted transfer resumes at
the byte the phone really has. Protocol and packet layout:
[docs/ble-note-sync.md](docs/ble-note-sync.md).

1. **Flash once over USB.** The Bluetooth build uses the 8 MB partition table
   (`partitions-epaper-8mb.csv`, two 3,968 KB slots); an over-the-air update cannot
   move partitions, so install the [release image](#install-a-release) once.
2. **Start the server.** `deploy/bluetooth-sync/` is the phone-facing half as two
   containers — the note endpoint and the relay page on one HTTPS origin, plus TLS so
   iOS will load it:

   ```sh
   cd deploy/bluetooth-sync && cp .env.example .env   # set RELAY_HOST, STT_URL, LLM_URL
   docker compose up -d --build
   ```

   Step by step, including the one-time certificate step for the iPhone:
   [deploy/bluetooth-sync/README.md](deploy/bluetooth-sync/README.md). Without a
   model it is still a server: it verifies uploads and keeps them.
3. **Pair the phone once.** Settings → **Phone app** (or `phone pair`) opens a
   two-minute window. The phone asks, the board names it on screen — *Allow this
   phone?* — and **nothing is shared until someone taps Allow**; then it shows a
   six-digit passkey and iOS asks you to confirm the same code. That acknowledgement
   is what keeps a phone in radio range from pairing itself with a device nobody is
   holding. `phone`, `status` and `phone allow`/`deny` cover it from the console.
4. **Connect → List notes → Send** on the page, in **Bluefy** on iOS (free, App
   Store; Safari has no Web Bluetooth). The page pulls the note in resumable,
   CRC-checked blocks, verifies the WAV's SHA-256, signs the upload with the board's
   HMAC and posts it. The panel shows `Bluetooth <n>%` while it runs, and `notes ble`
   shows the link, the MTU and why the last transfer stopped. **Fetch every note**
   drains the card in one go.

The whole first-time walk-through, with what each screen says:
[docs/ios-bluetooth-sync.md](docs/ios-bluetooth-sync.md).

The radio half is new and only partly exercised: the GATT table registering on real
NimBLE, the build and the host tests are verified, while an iPhone pairing, the MTU
iOS negotiates, throughput and a real note pulled off the card by a phone are **not**
— QA had no iPhone or BLE central. The relay page's own 11 Node cases rebuild the WAV
from a packet stream captured from the firmware, SHA-256 included, and the container
was exercised end to end with a real note and a real signature. Treat the first real
transfer as the test, with `notes ble` open.

## Known limitations

- An iPhone has not paired with this build, and a real note has not travelled over
  Bluetooth yet. See [§11](docs/ble-note-sync.md#11-what-has-been-verified-and-what-has-not).
- The page needs a secure origin: the container serves it over HTTPS, and a plain
  LAN `http://` address is refused. There is no native iOS app and no Shortcut that
  can do the transfer.

## Repository layout

```
firmware/core/      portable device core (UI, protocol, audio, app loop)
firmware/esp32/     ESP32-S3 application: board table, ports, modes, notes
firmware/tests/     host tests for the core and the e-paper modes
firmware/sim/       desktop simulator (same core)
plugin/             Hermes Gadget plugin, incl. the notes endpoint (port 8766)
python/             host-side client library (`hermes-gadget` CLI)
tests/              plugin and CLI test suites
docs/               SDK documentation; docs/audit.md is this device's audit
linux/              Raspberry Pi install/image tooling
site/               installer and documentation site
```

## CI

`.github/workflows/firmware.yml` builds `waveshare-epaper-154` with PlatformIO
and runs the host core tests on every push and pull request, then uploads the
flashable images as artifacts. Upstream's larger multi-board matrix is in
`.github/workflows/ci.yml` and can be enabled per board.

## Working in this repository

Read [CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md) first; they
cover documentation, verification, licensing, and contributor credit. Changes to
the device belong on feature branches off `main`.

## Lineage

Fork of [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk)
(MIT, "Hermes Gadget SDK contributors"), with the e-paper board port, the offline
voice-note stack, and the notes ingestion path added here. The upstream README is
kept as [README.upstream.md](README.upstream.md).

## License

This repository is MIT licensed ([LICENSE](LICENSE)); third-party notices are in
[NOTICE](NOTICE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and
[LICENSES/](LICENSES/).
