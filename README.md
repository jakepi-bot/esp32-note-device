# ESP32 Note Device (Jakebot)

A pocket e-paper device on the **Waveshare ESP32-S3-ePaper-1.54 (V2)**. It talks
to your own Hermes Agent, records voice notes with no network at all, and gets
those notes to your notes app either over Wi-Fi or over Bluetooth through your
phone.

```
  board ──Wi-Fi──▶ Hermes gateway ─────────────────▶ Obsidian
    │
    └──BLE──▶ iPhone (Bluefy page) ──HTTPS──▶ note server ──▶ Obsidian
```

| Press | What happens |
|---|---|
| **PWR**, short | start or end a live turn with Hermes |
| **PWR**, hold | power off (hardware latch) |
| **BOOT**, tap | start or stop a voice note on the card |
| **BOOT**, hold 2 s | on the home screen, open the menu; while recording, throw the note away; in the menu, leave it |
| **BOOT**, tap in the menu | next item |
| **PWR**, tap in the menu | change the item |

The panel returns to its home screen after 60 s with no input, and shows the
battery, the time, and how many notes are waiting.

Setup is four steps: flash the board, connect it to Hermes, deploy the note
server, pair the phone. Steps 3 and 4 are only needed for the Bluetooth path.
Wi-Fi uploads work with steps 1 and 2 alone.

## What you need

| | |
|---|---|
| The board | Waveshare ESP32-S3-ePaper-1.54 (V2), plus a USB cable that carries data |
| Wi-Fi | a 2.4 GHz network and its password |
| A computer with Hermes | Hermes Agent 0.21.6 or newer, gateway running |
| Docker | with the Compose plugin, for the note server (step 3) |
| An iPhone | with **Bluefy** from the App Store, for the Bluetooth path (step 4) |

You do not need Python, CMake, or a compiler to install a release. That is only
for [building from source](#build-from-source).

## Step 1 — flash the board

Plug the board into a computer and open the browser installer in Chrome or Edge:

**<https://jakepi-bot.github.io/esp32-note-device/>**

Pick the board, install, and leave the cable in until it says it is finished.
The installer keeps your settings if the board already has some.

Prefer the command line? Grab the full image from the
[releases page](https://github.com/jakepi-bot/esp32-note-device/releases/latest):

```bash
esptool.py --chip esp32s3 write_flash 0x0 hermes-gadget-waveshare-epaper-154-0.7.1.bin
```

Two things worth knowing:

- Bluetooth does not fit the old app slots, so the board uses the 8 MB partition
  table. Flash the **full image over USB once**. After that, updates can install
  over the air with `hermes gadget update <device> <file>`.
- Writing the full image erases the settings area, so the board pairs again as a
  new device. Keep its Wi-Fi password and key handy.

## Step 2 — connect it to Hermes

On the computer that runs Hermes:

```bash
hermes plugins install https://github.com/jakepi-bot/esp32-note-device/tree/main/plugin --enable
hermes gateway setup      # choose Hermes Gadget, restart the gateway when asked
hermes gadget info        # prints the device URL and an installer link
```

Install the `plugin` directory, not the repository root. Hermes scans everything
it installs and refuses the root.

Now give the board that address. The installer's next page takes it, or on the
board join Wi-Fi first: with no saved network it opens its own `Hermes-XXXX`
setup network (page at `http://192.168.4.1`). When the board shows a pairing
code, approve it:

```bash
hermes gadget pair
```

Then set the two things an out-of-the-box screen gets wrong:

```sh
set timezone "CST6CDT,M3.2.0,M11.1.0"   # your POSIX TZ, otherwise the clock reads UTC
set battery_estimate liion              # only with a single-cell Li-ion pack fitted
```

Until `battery_estimate` is set the footer says `NO BATTERY`, because the board
has no fuel gauge and will not guess at a cell that may not be there.

**You know it worked when** the board reaches Ready and a short press of PWR
gives you a spoken reply. Speech needs `stt:` and `tts:` configured in Hermes
(`hermes tools`, `hermes setup`, or `config.yaml`). Full detail, including
over-the-air updates and phone-based Wi-Fi setup:
[docs/connect-hermes.md](docs/connect-hermes.md) and
[docs/setup-board.md](docs/setup-board.md).

## Step 3 — deploy the note server

This is the phone-facing half: an endpoint that takes a note, transcribes it,
and writes it into your vault. Two containers, one HTTPS origin.

```bash
git clone https://github.com/jakepi-bot/esp32-note-device.git
cd esp32-note-device/deploy/bluetooth-sync
cp .env.example .env
```

Edit `.env`. Three values matter:

```ini
RELAY_HOST=hermes.local          # the name the phone will type. A name, not an IP.
STT_URL=http://192.168.1.20:8081/v1     # any OpenAI-compatible /v1/audio/transcriptions
LLM_URL=http://192.168.1.20:11434/v1    # any OpenAI-compatible /v1/chat/completions
```

Leave the two model URLs empty and it still works as a server. It verifies each
upload and keeps the audio, it just does not transcribe. If a Hermes gateway on
this machine already owns port 8766, set `NOTES_PORT=8767`.

Make the folders, hand it the board's key, and start it:

```bash
mkdir -p data/notes data/vault data/gadget
cp ~/.hermes/plugin-data/gadget/devices.json data/gadget/devices.json
docker compose up -d --build
docker compose logs -f note-sync
```

Without a key file yet, enroll the board's key directly (`hermes gadget devices`
shows it):

```bash
docker compose run --rm --entrypoint python note-sync /app/serve.py enroll --key '<base64 key>'
```

Check both listeners:

```bash
curl -s http://localhost:8766/health                          # {"status":"ok", ...}
curl -sk -o /dev/null -w '%{http_code}\n' https://localhost:8443/   # 200
```

**You know it worked when** the log says `complete:` (or `stage only:` with no
transcriber), and both curls answer. The long version, including how to point it
at your real Obsidian vault: [deploy/bluetooth-sync/README.md](deploy/bluetooth-sync/README.md).

## Step 4 — pair the phone

iOS only runs Web Bluetooth on HTTPS, so the phone has to trust the server's
certificate once.

```bash
docker compose cp relay:/data/caddy/pki/authorities/local/root.crt ./caddy-root.crt
```

Send `caddy-root.crt` to the iPhone (AirDrop or email), open it, then:

1. **Settings → General → VPN & Device Management** → the profile → **Install**.
2. **Settings → General → About → Certificate Trust Settings** → turn on
   **Caddy Local Authority**.

Now pair, with the board in your hand:

1. On the board: hold **BOOT** for two seconds to open the menu, then tap **BOOT**
   down to **Phone app** and tap **PWR** on it. The screen says *Pair a phone* and
   the window stays open for two minutes.
2. On the iPhone, open `https://hermes.local:8443/` in **Bluefy**. Use the same
   name you put in `RELAY_HOST`.
3. Type the **device id** (`hg-…`) and the **device key** (base64) into the page.
   Both are in `hermes gadget devices`, or in the enroll output.
4. Tap **Connect** and pick the board.
5. The board asks *Allow this phone?*. **Press BOOT to allow. PWR refuses, and 20
   seconds with no answer refuses.**
6. iOS shows a six-digit code and the board shows the same one. Confirm only if
   they match.

**You know it worked when** the page lists the notes waiting on the card.

After that, pairing is remembered: open the page, tap **Connect**, pick the notes
and tap **Send**. Nothing is deleted from the board until the server answers 2xx,
and an interrupted transfer resumes where it stopped. Walk-through with every
screen: [docs/ios-bluetooth-sync.md](docs/ios-bluetooth-sync.md).

## Day to day

- Record with a BOOT tap. No network needed at any point.
- On Wi-Fi, the board drains `pending/` to Hermes by itself.
- Off Wi-Fi, open the page in Bluefy and tap **Fetch every note**.
- The card keeps synced notes for 14 days and deletes the oldest first when it
  gets under 10% free.
- USB console commands, if you want them: `notes`, `notes list`, `notes sync`,
  `notes ble`, `phone`, `diag`, `help`.

## If it does not work

| What you see | What it is |
|---|---|
| Board shows `NO TIME` | `timezone` is not set (step 2) |
| Footer says `NO BATTERY` | `battery_estimate liion` is not set (step 2) |
| Bluefy says Web Bluetooth is unavailable | the page is not on HTTPS, or the certificate is not trusted (step 4) |
| **Connect** finds no device | the pairing window closed, or the board is asleep. Open Settings → Phone app again |
| **Connect** fails with `authentication` | the board did not allow this phone. Open the window and press BOOT when it asks |
| `bind: address already in use` on 8766 | a Hermes gateway owns that port. Set `NOTES_PORT=8767` |
| `401 device … is not enrolled` | the server has no key for that id. Step 3, the `devices.json` copy |
| Log says `stage only` | no `STT_URL`, so notes are kept untranscribed in `data/notes/inbox` |

More: [docs/troubleshooting.md](docs/troubleshooting.md) and the table at the end
of [deploy/bluetooth-sync/README.md](deploy/bluetooth-sync/README.md).

## Where the detail lives

| Document | Covers |
|---|---|
| [docs/ble-note-sync.md](docs/ble-note-sync.md) | Bluetooth protocol, what the board does with each block |
| [docs/ios-bluetooth-sync.md](docs/ios-bluetooth-sync.md) | the phone side, screen by screen |
| [docs/voice-note-ingestion.md](docs/voice-note-ingestion.md) | the `POST /notes/v1` wire contract |
| [docs/hermes-integration.md](docs/hermes-integration.md) | plugin settings, ports, profiles |
| [docs/hardware.md](docs/hardware.md) | board, pinout, flashing by hand |
| [docs/using-gadget.md](docs/using-gadget.md) | every control and setting |
| [docs/audit.md](docs/audit.md) | this board's hardware assumptions |
| [docs/qa-report-0588422.md](docs/qa-report-0588422.md) | hardware QA, defects and what is verified |

Status: release 0.7.1 builds from a clean checkout, is flashed on a physical
board, and has been validated end to end over Wi-Fi (pairing, live turns, offline
notes uploaded to the notes endpoint). The Bluetooth half is the newest: the GATT
table registers on real NimBLE and the relay page rebuilds a captured packet
stream byte for byte, but treat your first phone transfer as the test and keep
`notes ble` open. [What has been verified](docs/ble-note-sync.md#11-what-has-been-verified-and-what-has-not).

## Build from source

```bash
pip install platformio==6.2.0
cd firmware/esp32
pio run -e waveshare-epaper-154            # build
pio run -e waveshare-epaper-154 -t upload  # flash over USB-C
pio run -e waveshare-epaper-154 -t monitor # serial console, 115200
```

PlatformIO pulls ESP-IDF 6.1 on the first build. ESP-IDF 6.1 works directly too,
see [docs/hardware.md](docs/hardware.md#build-and-flash). The browser installer
and `hermes gadget update` are for released images, not for source builds.

```
firmware/core/      portable device core (UI, protocol, audio, app loop)
firmware/esp32/     ESP32-S3 application: board table, ports, modes, notes
firmware/tests/     host tests for the core and the e-paper modes
plugin/             Hermes Gadget plugin, including the notes endpoint on 8766
python/             host-side client library (the `hermes-gadget` CLI)
deploy/             the Bluetooth note server (Docker Compose)
web/ble-note-relay/ the phone page, its Node tests and fixture
docs/               all documentation
```

CI builds `waveshare-epaper-154` with PlatformIO and runs the host tests on every
push. Contributions: read [CONTRIBUTING.md](CONTRIBUTING.md) and
[AGENTS.md](AGENTS.md) first.

## Lineage

A focused fork of [Adolanium/hermes-gadget-sdk](https://github.com/Adolanium/hermes-gadget-sdk)
(MIT). The upstream tree is kept whole so upstream fixes merge instead of being
re-ported, and its README is [README.upstream.md](README.upstream.md).

## License

This repository is MIT licensed ([LICENSE](LICENSE)), with third-party notices in
[NOTICE](NOTICE), [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and
[LICENSES/](LICENSES/).
