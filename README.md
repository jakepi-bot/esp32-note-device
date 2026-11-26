# ESP32 Note Device

A pocket voice-notes recorder on the **Waveshare ESP32-S3-ePaper-1.54 (V2)**.
Press one key, speak, and the note is on the microSD card. It goes to your notes
app on its own over Wi-Fi, or through your phone over Bluetooth. Nothing is lost
if neither is reachable: the card keeps the recording until an upload succeeds.

```
  board ──Wi-Fi (window only)──▶ POST /notes/v1 ──▶ transcription ──▶ Obsidian
    │
    └──BLE──▶ phone page ──HTTPS──▶ note server ──▶ transcription ──▶ Obsidian
```

The firmware this repository ships for the device is the **notes-only** app: the
Hermes Gadget core, its turn machinery and its generic UI are not in the image at
all. It is built by the PlatformIO environment `notes` and measures 1,464,915 B
(36.1% of the 3.87 MB slot) with 63,040 B of static RAM.

## What it does

- Records a voice note to the card with no network at all: 24 kHz PCM16 WAV plus a
  `note.json`, in `notes/pending/` until the server acknowledges it.
- Uploads over Wi-Fi only inside a window: at boot when notes are waiting or the
  clock is unset, after a note is recorded, from **Sync now**, or while a phone's
  setup page is open. The station is off outside a window.
- Syncs the clock over SNTP while the station is up, so notes are dated, not 1970.
- Serves notes to a phone over Bluetooth LE (a GATT note service, paired by
  passkey after an on-screen ask), so a phone can carry a note out and post it.
- Keeps its own card tidy: synced notes are deleted after 14 days, or oldest first
  when the card is under 10% free. Pending and failed notes are never deleted by
  age.
- Shows the queue on a 200x200 e-paper face, and a menu with the note list,
  playback through the onboard speaker, delete, sync, the phone window, Wi-Fi
  setup and About.
- Powers off gracefully on a PWR tap: the recording is committed, the card is
  flushed and unmounted, the radio is stopped.

## What it does not do

The Hermes live link is gone from the device. Concretely, the image has **no**
WebSocket client, `hg::App`, UI model, i18n table, mascot or faces, Opus,
prompt or choice cards, dashboards, widgets, profiles, timers, camera, tailnet or
touch. It does not talk to Hermes, does not run turns, does not speak replies,
and does not draw anything an agent pushed to it. The panel is the notes app's own
nine screens and nothing else.

The hardware has no battery-backed clock, which is why a note recorded before the
first Wi-Fi window can carry a 1970 stamp until the upload gives it a real one.

## The host still runs Hermes, for transcription

The device records audio; turning audio into a text note happens on the host. The
default path for it is the Hermes Gadget plugin's own endpoint, `POST /notes/v1`
on port 8766: it verifies the device's HMAC, stages the audio, transcribes it with
the profile's speech-to-text, summarizes it with the profile's model, and writes
the note into `~/obsidian/voice notes/`.

So a Hermes Agent host (0.21.6 or newer for the plugin) is required for the full
flow, but the device never opens a socket to the gateway, never pairs with
Hermes, and keeps working with the gateway stopped as long as the note server it
posts to is up. The alternative server in this repository,
[`deploy/bluetooth-sync`](deploy/bluetooth-sync/README.md), transcribes through
any OpenAI-compatible `STT_URL` and needs no Hermes at all.

## Keys

Two keys, one job each. PWR is power everywhere except inside the menu, where it
selects (and it is the "no" on a delete). BOOT is the work key.

| Gesture | Home | Recording | Menu | Note list | A note | Delete? | Playing | Pairing ask |
|---|---|---|---|---|---|---|---|---|
| **PWR** tap | power off | power off | select the row | open the note | select the action | keep the note | power off | refuse the phone |
| **BOOT** tap | start recording | stop and keep | next row | next note | next action | keep the note | stop | allow the phone |
| **BOOT** hold 2 s | open the menu | discard the note | back to Home | back to the menu | back to the list | **delete the note** | stop | nothing |
| **PWR** hold | hardware latch off, always | | | | | | | |

The delete confirmation is the hold, with the note's own date on the glass:
**hold BOOT to delete, tap to keep**. Deleting a note removes the card's copy
only; a note already in the vault stays there.

## Menu

```
Notes (12)      newest first: date, length, and a * or ! mark
Sync now        connect, upload every pending note, come back to Home
Phone           the pairing and transfer window for the phone page
Wi-Fi           the phone's setup page
About           battery, firmware, device id, notes on the card
Back
```

## Build and flash

PlatformIO builds two environments from this tree. `notes` is the device's
firmware; `waveshare-epaper-154` is the Hermes core app, kept buildable as a
regression check until the notes app has passed the same bench run.

```bash
pip install platformio==6.2.0
git clone https://github.com/jakepi-bot/esp32-note-device.git
cd esp32-note-device/firmware/esp32
pio run -e notes                 # build
pio run -e notes -t upload       # flash over USB-C
pio run -e notes -t monitor      # serial console, 115200
```

PlatformIO fetches ESP-IDF 6.1 and the managed components on the first build (a
warm package cache takes about two minutes). The build prints the app size against
the 3.87 MB slot; `tools/check_size.py` fails it if a slot would drop under 10%
free. [Clean-build verification](docs/build-verification.md) records the numbers
from a fresh clone.

The board uses 8 MB of flash and the 8 MB partition table the `notes` environment
already sets (`partitions-epaper-8mb.csv`), because the image does not fit the
shared 1.9 MB slots. Flashing the full image over USB once is what moves a board
from an older partition table; settings in `nvs` keep their offset across that
change.

### Flashing a released image

The browser installer at **<https://jakepi-bot.github.io/esp32-note-device/>**
writes the merged image from a release, and the direct route is the same file
through esptool:

```bash
esptool.py --chip esp32s3 write_flash 0x0 hermes-gadget-waveshare-epaper-154-0.7.1.bin
```

That image is the **Hermes app**, not the notes app: 0.7.1 is the last release cut
from this repository before the notes-only rewrite, and no release of the notes
firmware has been tagged yet. When one is, it comes from the same workflow
(`firmware/esp32/tools/package_release.py`, which deliberately excludes the
`notes` variant from the release's board list).

### Over-the-air updates

Not wired in the notes app yet. `port_ota` implements ESP-IDF's OTA API with
rollback and is used by the Hermes app (`hermes gadget update` pushes an app image
over the live link), but the notes app has no live link to carry one: nothing in
`firmware/esp32/notes_app/` calls `EspUpdater`, and there is no HTTP manifest
pull. Today the notes firmware is updated over USB with
`pio run -e notes -t upload`. Decision 3 of
[the plan](docs/notes-only-firmware-plan.md) replaces the WebSocket `ota.*`
messages with a small HTTP manifest pull; that is not written yet.

## Provision Wi-Fi

A board with no saved network is perfectly usable: it records and holds notes. To
give it a network, open its setup access point.

1. On the board, hold **BOOT** for two seconds to open the menu, BOOT-tap down to
   **Wi-Fi**, and PWR-tap to select it. Over USB, `wifi-setup` on the console does
   the same and prints the credentials.
2. The board starts an access point named `Hermes-XXXX` with a random 8-character
   password, shown on the setup screen and printed by `wifi-setup`. It stays up
   for ten minutes; `wifi-setup stop` closes it.
3. Join that network from a phone or laptop. Keep it selected if the device warns
   that it has no internet.
4. Open `http://192.168.4.1` and enter the 2.4 GHz network name, its password, and
   the Hermes address from `hermes gadget info`, for example
   `ws://hermes.local:8765/gadget` or `ws://192.168.1.20:8765/gadget`. The notes
   app uses only the **host** of that address: it posts to
   `http://<host>:8766/notes/v1`. To point somewhere else, set the URL explicitly
   on the console with `set notes_url http://host:port/notes/v1`.
5. Choose **Check connection and save**. The board saves the network only after it
   holds an address on it; a failed attempt keeps the previous settings.

The page also offers a Tailscale auth key in the Hermes app. The notes build has
no tailnet, so that field is not shown.

## Console

The USB serial console at 115200 answers these on the notes firmware:

| Command | What it does |
|---|---|
| `notes status` | queue counts, endpoint, clock, card state |
| `notes list` | the notes on the card, newest first |
| `notes record`, `notes stop` | record without touching the keys |
| `notes sync` | run an upload window now |
| `notes retry <id>` | put a note from `failed/` back in the queue for one pass |
| `notes poweroff` | the graceful shutdown a PWR tap performs |
| `notes mount`, `notes format`, `notes prune` | card housekeeping |
| `screen` | the panel image as hex, 1 bit per pixel (no camera needed) |
| `about`, `battery`, `clock`, `radio`, `sd`, `keys`, `ui` | the pieces of the status face; `ui` reports the screen and its counts, `ui home` redraws it |
| `phone pair`, `phone wake` | open the pairing window, or a short window to look for a phone |
| `phone allow`, `phone refuse` | answer the pairing ask from a host instead of the keys |
| `wifi-setup`, `wifi-setup stop` | the phone's setup page |
| `set`, `get`, `diag`, `log`, `help` | settings, diagnostics, logs |

`set` accepts `server`, `token`, `name`, `notes_url`, `timezone` on this build.

## Phone transfer

The phone page (`web/ble-note-relay/`) is what carries a note out over Bluetooth:
it pairs with the board, fetches the notes, and posts each one to the server,
which verifies the signature and transcribes it. The board allows a phone only
after the on-screen ask - **BOOT allows, PWR refuses, 20 s of neither refuses** -
and only a paired phone can read a note.

1. On the board: hold **BOOT** for two seconds, BOOT-tap down to **Phone**, and
   PWR-tap to select. The window stays open for two minutes.
2. On the phone, open the server's page: `https://hermes.local:8443/` in Bluefy
   on iOS, or the same URL in Chrome on Android. Use the name in `RELAY_HOST`.
3. Type the **device id** (`hg-...`) and the **device key** (base64) if the page
   asks for them; both are in `hermes gadget devices`. The page stores them in
   `localStorage` because it signs the upload itself.
4. Tap **Connect** and pick the board. The board asks *Allow this phone?*; press
   **BOOT**. Confirm the six-digit code only when it matches the one on the board.
5. Pick the notes and tap **Send**. Nothing is deleted from the board until the
   server answers 2xx, and an interrupted transfer resumes where it stopped.

The server is either the Hermes plugin ([Hermes integration](docs/hermes-integration.md))
or the standalone container:

```bash
git clone https://github.com/jakepi-bot/esp32-note-device.git
cd esp32-note-device/deploy/bluetooth-sync
cp .env.example .env      # RELAY_HOST, STT_URL, LLM_URL; NOTES_PORT=8767 if 8766 is taken
mkdir -p data/notes data/vault data/gadget
cp ~/.hermes/plugin-data/gadget/devices.json data/gadget/devices.json
docker compose up -d --build
```

- iPhone: [Sync notes from an iPhone](docs/ios-bluetooth-sync.md). Web Bluetooth
  runs on HTTPS only, so the server's local certificate is trusted once.
- Android: [Sync notes from an Android phone](docs/android-bluetooth-sync.md).
  Chrome, the Nearby devices permission, and the same page.

## Day to day

- Record with a BOOT tap. No network needed at any point.
- The face reads `ALL SENT` or `N TO SEND`, the battery, the last synced note,
  and `BOOT: record`. During a window it reads `UPLOADING`; with nothing
  reachable, `OFFLINE`.
- The card keeps synced notes for 14 days and deletes the oldest first when it
  gets under 10% free.
- A PWR tap powers off: the recording is kept, the card is flushed, the radio is
  stopped.

## If it does not work

| What you see | What it is |
|---|---|
| `NO TIME` | the clock is unset: it needs one Wi-Fi window with SNTP (the band says so until it is set) |
| `OFFLINE` | no window is open, or nothing answers at the notes endpoint |
| `NO SYNC` | uploads are failing: check `notes status` and the server's log |
| `NO CARD` | the microSD card is missing or did not mount |
| `LOW POWER` | the battery is nearly flat |
| `!` on a list row | the server rejected that note (it is in `failed/`); the reason is in its `note.json` |
| the list says `128+` | the on-screen window stops at 128 entries; every note is still on the card |
| the page says **Connect** lists no device | the pairing window is closed. The board advertises only while **Phone** is open; `python3 web/ble-note-relay/tools/scan_adv.py` shows the packet |
| iOS Settings lists no gadget | expected: the board is found inside the page, not in iOS Settings |
| `bind: address already in use` on 8766 | a Hermes gateway owns that port. Set `NOTES_PORT=8767` |
| `401 device ... is not enrolled` | the server has no key for that id: copy `devices.json` or enroll the key |

More: [Troubleshooting](docs/troubleshooting.md), and the table at the end of
[deploy/bluetooth-sync/README.md](deploy/bluetooth-sync/README.md).

## Where the detail lives

| Document | Covers |
|---|---|
| [Using the notes device](docs/notes-device.md) | every screen, key and state, as built |
| [The plan behind the rewrite](docs/notes-only-firmware-plan.md) | what was dropped, what was kept, the measured before and after |
| [The UI specification](docs/notes-ui-spec.md) | the screens, the copy, the e-paper rules |
| [The notes UI, as built](docs/notes-ui-implementation.md) | which file owns which rule, and how it is tested |
| [ePaper 1.54 integration spec](docs/waveshare-epaper-154-v2.md) | board, panel, audio, storage, keys |
| [Voice-note upload contract](docs/voice-note-ingestion.md) | the `POST /notes/v1` wire format |
| [Bluetooth note sync](docs/ble-note-sync.md) | the GATT service and the transfer protocol |
| [Sync notes from an iPhone](docs/ios-bluetooth-sync.md) | the phone side, screen by screen |
| [Sync notes from an Android phone](docs/android-bluetooth-sync.md) | Chrome for Android, permissions, the same page |
| [Hardware and wiring](docs/hardware.md) | the board, pinout, flashing by hand |
| [Clean-build verification](docs/build-verification.md) | the build from a fresh clone, with numbers |
| [Low power](docs/low-power.md) | the radio windows and the doze rules |

## Status

The notes-only firmware builds from a clean checkout and has been run on the
physical board. Recorded there: boot to the Home face, a Wi-Fi window that joins
and syncs the clock over SNTP, a recorded note uploaded with an HMAC the gateway
verified (`HTTP 202`), a phone bonded by passkey after the on-screen ask, and a
graceful power-off that keeps the card's notes. A QA pass then proved one thing
the bench pass had missed: the screens were built once at boot and never
refreshed. That fix is in flight, and [CHANGELOG.md](CHANGELOG.md) records the
state of each piece. No release of the notes firmware has been tagged yet, so
`pio run -e notes` is the way to get it.

Known gaps: no on-device OTA, and the setup page still asks for a Hermes address
because it is the shared port's page (see **Provision Wi-Fi**).

## Layout

```
firmware/core/        portable device core (Hermes UI, protocol, audio, app loop)
firmware/esp32/       ESP32-S3 application: boards, ports, notes, the notes app
firmware/tests/       host tests for the core, the modes and the notes UI
plugin/               Hermes Gadget plugin, including the notes endpoint on 8766
python/               host-side client library (the `hermes-gadget` CLI)
deploy/               the Bluetooth note server (Docker Compose)
web/ble-note-relay/   the phone page, its Node tests and fixture
docs/                 all documentation
```

CI builds `-e notes` and `-e waveshare-epaper-154` and runs the host tests on
every push. Contributions: read [CONTRIBUTING.md](CONTRIBUTING.md) and
[AGENTS.md](AGENTS.md) first.

## Lineage

A focused fork of [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk)
(MIT). The SDK tree is kept whole, including the other board profiles and the
Hermes core, so upstream fixes merge instead of being re-ported; this device
builds only the two environments named above. Upstream's README is
[README.upstream.md](README.upstream.md).

## License

This repository is MIT licensed ([LICENSE](LICENSE)), with third-party notices in
[NOTICE](NOTICE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and
[LICENSES/](LICENSES/).
