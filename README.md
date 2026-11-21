# ESP32 Note Device (Jakebot)

Firmware for a pocket voice-note and assistant device built on the **Waveshare
ESP32-S3-ePaper-1.54 (V2)**: hold BOOT to talk to Hermes, tap PWR to record an
offline voice note to the microSD card, and let the device upload the queue to
Hermes whenever it is next online.

This repository is the canonical home of that firmware. It is a focused fork of
the [Hermes Gadget SDK](https://github.com/Adolanium/hermes-gadget-sdk) (MIT) and
keeps the upstream tree whole on purpose: every board target, the shared device
core, the host tests, the Hermes plugin, and the documentation still build here,
so upstream fixes can be merged instead of re-ported. The target board to build
and flash is `waveshare-epaper-154`.

Status (2026-11-20): firmware 0.6.0 builds from a clean checkout, is flashed on a
physical board, and has been validated end to end over Wi-Fi — pairing to a live
Hermes hub, live assistant turns (record → stream → reply → spoken reply), and
offline voice notes recorded to the card and uploaded to the notes endpoint.
Hardware detail and assumptions are in [docs/AUDIT.md](docs/AUDIT.md).

## Hardware

| Part | Detail |
|---|---|
| SoC | ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB octal PSRAM, Wi-Fi 2.4 GHz, BLE 5 capable |
| Display | 1.54" 200×200 1-bit e-paper, SSD1681-class controller, SPI, partial refresh |
| Audio | ES8311 codec (DAC + ADC on one I2S bus) → NS4150B amplifier → 8 Ω speaker, analog mic |
| Storage | microSD, 1-bit SDIO, FAT32 |
| Buttons | BOOT (GPIO 0) = TALK; PWR (GPIO 18) = voice-note key / power |
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

## Notes: what the device does

- **Tap PWR** → record; **tap again** → stop and commit the note to the card.
  No network is needed at any point.
- Each note is a folder `notes/<state>/<note_id>/` holding `recording.wav`
  (24 kHz, PCM16, mono) and `note.json` (metadata), where `<state>` is one of
  `pending`, `sent`, `failed`.
- The uploader drains `pending/` to `POST /notes/v1` (multipart) whenever the
  device is online and the clock is set, with exponential backoff and a durable
  queue, so a power cut loses nothing. `sent/` keeps the acknowledged notes,
  `failed/` keeps server rejections for inspection.
- Note ids are `<device_id>-<YYYYMMDDTHHMMSS>-<seq>`, which sorts by time and
  doubles as the idempotency key (retries are byte-identical).

Wire contract: [docs/voice-note-ingestion.md](docs/voice-note-ingestion.md).
Card layout and uploader design: [docs/waveshare-epaper-154-v2.md](docs/waveshare-epaper-154-v2.md) §7.

Console commands (type `help` for everything else):

| Command | Effect |
|---|---|
| `notes` | counts, card state, target URL, mode, queued bytes |
| `notes record` / `notes stop` | record from the console |
| `notes sync` | force an upload pass |
| `notes list` | ids per state |
| `notes delete <id>` / `notes delete --failed` | remove notes |
| `notes mount` / `notes format` | remount or format the card |
| `screen` | hex dump of the 1-bit panel image (proves what is shown) |
| `diag` | tasks, stacks, memory, i2c, display controller |

## Repository layout

```
firmware/core/      portable device core (UI, protocol, audio, app loop)
firmware/esp32/     ESP32-S3 application: board table, ports, modes, notes
firmware/tests/     host tests for the core and the e-paper modes
firmware/sim/       desktop simulator (same core)
plugin/             Hermes Gadget plugin, incl. the notes endpoint (port 8766)
python/             host-side client library (`hermes-gadget` CLI)
tests/              plugin and CLI test suites
docs/               SDK documentation; docs/AUDIT.md is this device's audit
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

## Lineage and license

Fork of [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk)
(MIT, "Hermes Gadget SDK contributors"), with the e-paper board port, the offline
voice-note stack, and the notes ingestion path added here. The upstream README is
kept as [README.upstream.md](README.upstream.md). This repository is MIT licensed
([LICENSE](LICENSE)); third-party notices are in [NOTICE](NOTICE),
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and [LICENSES/](LICENSES).
