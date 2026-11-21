# Waveshare ESP32-S3-ePaper-1.54 v2 × Hermes Gadget — integration spec

Status: research + design, no code yet. Written for the fork that implements the
"AI assistant / voice note taker" device (live assistant mode + offline SD voice notes
summarized into Obsidian).

Board: Waveshare **ESP32-S3-ePaper-1.54**, hardware revision **v2** (module
ESP32-S3-PICO-1-N8R8), 8 MB flash + 8 MB octal PSRAM, 1.54″ 200×200 e-paper,
onboard ES8311 codec + NS4150B amp, TF slot, two buttons, Li-ion charger.
Doc link on Waveshare's site: <https://docs.waveshare.com/ESP32-S3-ePaper-1.54>.

Facts below were read from vendor code/docs and this repo at
`da48d2570553ff3a8090e32b0eab0c9eb71a7684` (upstream `main`, plugin 0.6.0 — the
same version installed on this host). Items marked **[verify]** need a measurement
on the physical board.

---

## 1. Decisions

| # | Decision | Rationale |
|---|---|---|
| D1 | Port this SDK as a new board (`waveshare-esp32s3-epaper-154`) instead of writing firmware from scratch. | The gadget plugin, protocol, auth, pairing, OTA, voice pipeline and UI already exist; `docs/porting.md` covers exactly this. |
| D2 | Transport is **Wi-Fi only** for talking to Hermes. BLE stays off the table for v1. | The gadget firmware has no Bluetooth stack (`docs/hardware.md`: "Boards whose firmware enables Bluetooth: none yet"). BLE exists only in other firmwares (Pika Note uses BLE + an Android app). Offline recording works without any transport, which is the only "no connection" behaviour v1 must satisfy. |
| D3 | Voice notes upload over **HTTP `POST` to a new endpoint inside the gadget plugin**, not over the gadget WebSocket. | The WS path is owned by the SDK core (forking it costs an upstream merge on every pull), the dashboard's plugin API runs in a *different* process behind dashboard auth, and an HTTP POST is what the ingestion task's integration test wants (`curl` with a WAV). Live voice keeps using the unmodified WS session. |
| D4 | Note audio is **WAV PCM16 mono @ 24 000 Hz**, max 5 min default (10 min hard cap). | `CodecAudio::kRate` is 24 000 Hz and `CodecAudio::start()` warns and refuses any other rate, so 24 kHz is the only rate the ES8311 path gives us. WAV needs no decoder on the server. |
| D5 | Obsidian destination: **`~/obsidian/Notes/voice notes/`** (note file), audio copy in `voice notes/_audio/<same stem>.<ext>`. | The vault has `Notes/` with subfolders (`AI Process`, `Attachments`, `Hermes`, `Jobs`); there is no voice-notes folder yet. Lowercase `voice notes` is the contract's canonical spelling — on a case-sensitive filesystem a `Voice Notes` folder would be a second vault. Root `Voice.md` is a writing-style note — do not collide with it. |
| D6 | The repo for firmware + spec is this checkout: **`hermes-gadget-sdk`** (clone of `Adolanium/hermes-gadget-sdk`, `main`, 1 commit behind nothing). | No repo existed for this project; the SDK is the codebase we extend, and sibling tasks (firmware foundation, ingestion pipeline, flash/validation) all need one shared path. |
| D7 | Companion/phone side = **the Web Bluetooth relay page on an iPhone** (Bluefy), served by the Docker container in `deploy/bluetooth-sync`; no native app. | Per Jake: he works from an iPhone. iOS has no BLE API for web pages outside a Web Bluetooth browser, so the phone side is a page, not an app: it reaches the radio, signs the upload with the board's key and posts it, and nothing has to be installed beyond Bluefy. Pairing a new phone needs an acknowledgement on the device itself. |

---

## 2. Target hardware

### 2.1 What is on the board (v2)

- **SoC** ESP32-S3-PICO-1-**N8R8**: 8 MB flash, 8 MB octal PSRAM, dual-core 240 MHz,
  Wi-Fi 2.4 GHz + BLE 5. (Waveshare product overview: "Flash 8 MB · PSRAM 8 MB",
  "supports 2.4 GHz Wi-Fi and BLE dual-mode".)
- **Display** 1.54″ e-paper **200×200**, 4-wire SPI, partial refresh supported, image
  retained with power off, deep sleep 2 µA; panel update consumes **8 mAs**
  (datasheet §2.4/§2.6). Driver code in the vendor demo drives commands
  `0x12/0x01/0x11/0x3C/0x18/0x22/0x20` with a 153-byte LUT (`0x32`) and separate
  full/partial LUTs — i.e. an SSD1681-class controller. **[verify]** exact panel
  part + measured full/partial refresh times.
- **Audio** **ES8311** codec (ADC + DAC, mono, mic input is the codec's own ADC —
  no ES7210 on this board) → **NS4150B** amp → 8 Ω 1 W speaker; mic input on the
  MX1.25 speaker header per "Speaker for voice playback (connect via onboard
  MX1.25 header)". Codec on I2C, I2S with MCLK.
- **Storage** TF card slot, FAT32, 1-line SDMMC (vendor demo wires D0/CLK/CMD only;
  the 4-bit series resistors are unpopulated on the schematic).
- **Sensors** SHTC3 temperature/humidity @0x70, **PCF85063** RTC @0x51 (both on the
  shared I2C bus), green LED.
- **Buttons** `BOOT` and `PWR` (Waveshare: "Boot: Download/Boot button ·
  PWR: Power button"). The vendor factory firmware reads **both** as buttons
  (`new Button(BOOT_BUTTON_PIN)` / `new Button(PWR_BUTTON_PIN)` and it waits for the
  PWR release at boot), and uses **double-press PWR** as a UI gesture — so GPIO18 is
  firmware-readable as a tap, not just a hardware latch.
- **Touch** — the product line has four SKUs: `ESP32-S3-ePaper-1.54`, `-EN`, and
  `ESP32-S3-Touch-ePaper-1.54`, `-EN`. Only the Touch SKUs carry the FT6336
  controller ("this example is only available for the touch-enabled version";
  V1 had no touch variant). This spec assumes the **non-touch** SKU (the task links
  the non-Touch page) — **[verify by eye; if it is the Touch SKU, set
  `touch_screen = true`, FT6336 on the shared I2C bus, `EPD_TP_RST` net, and re-read
  §6]**.
- **Power** USB-C 5 V in, onboard charger (**ETA6098**, schematic), 3.3 V buck
  (MP1605), single-cell Li-ion on MX1.25 2-pin, battery-voltage divider to ADC,
  VBAT load switch driven by a GPIO (power latch).
- **Connectors** 40-pin FPC (DVP camera, UART, GPIO, SD, I2S, e-paper breakout),
  battery header, speaker header, UART header, USB-C.

### 2.2 Pin map (authoritative source: vendor ESP-IDF demo, `main/user_config.h` +
`components/codec_board/board_cfg.txt`, cross-checked against the schematic PDF)

| Function | GPIO | Notes |
|---|---|---|
| e-paper SCK / MOSI | 12 / 13 | SPI2_HOST, CS 11, D/C 10 |
| e-paper CS / D/C / RST / BUSY | 11 / 10 / 9 / 8 | BUSY active level: `LOW: idle, HIGH: busy` per vendor driver comment **[verify against panel]** |
| e-paper power rail | 6 | `EPD_PWR_PIN`, gate before first touch of the panel |
| Audio rail (codec VDD) | 42 | `Audio_PWR_PIN`, must be high before `g_codec.begin()` |
| Speaker amp "pa" | 46 | `codec_board` cfg `pa: 46` (schematic net `PA_CTRL`/`PA_EN` label the pair as 42/46 — drive 42 as rail, 46 as pa, then confirm audibly) **[verify]** |
| I2S MCLK / BCLK / LRCK | 14 / 15 / 38 | from `board_cfg.txt` `i2s: {bclk: 15, ws: 38, dout: 45, din: 16, mclk: 14}` |
| I2S DAC out (to PA) / ADC in (mic) | 45 / 16 | `use_mclk: 1`, `in_out: {codec: ES8311}` |
| I2C SDA / SCL | 47 / 48 | shared bus: ES8311, PCF85063 @0x51, SHTC3 @0x70 |
| SDMMC CLK / CMD / D0 | 39 / 41 / 40 | 1-line SDMMC, `SDMMC_HOST_SLOT_0` |
| Battery ADC | 4 | `ADC1_CHANNEL_3`, demo computes `VBAT = mV * 2` (÷2 divider) |
| VBAT power latch | 17 | `VBAT_PWR_PIN`; assert high at boot or the board dies on battery |
| PWR button | 18 | `PWR_BUTTON_PIN`; also the hardware power key |
| BOOT button | 0 | `BOOT_BUTTON_PIN`, also flash-download and deep-sleep wake (`ext_wakeup_pin_1`) |
| RTC interrupt | 5 | schematic `RTC_INT` (alarm wake for scheduled doze/clock) |
| Green LED | 3 | schematic `LED_G` **[verify active level]** |

Nothing else on the board needs a pin: no touch, no camera used, UART header free
(U0TXD/U0RXD available if you give up the USB console).

### 2.3 Precedent and community projects

- **Pika Note** <https://github.com/zaera/Pika_note> — same board, Android-only BLE
  companion app: phone does the Wi-Fi setup and provisioning, firmware records the
  voice note over BLE and a **private Google-Drive endpoint** transcribes it. Useful
  for its A1/A4/A5/A9 **hardware wiring tables** (they confirm the pin map above) and
  for "note folder + priority levels + browse/copy/delete from the phone" UX. Not
  usable for us: no iOS app, no Hermes. Ancestor of the Pala Note e-reader voice
  note system (see Paul Lagier's talk linked from Waveshare's Resources page).
- **Waveshare's own examples** — `waveshareteam/ESP32-S3-ePaper-1.54`:
  Arduino + ESP-IDF demos (`02_Example/ESP-IDF/V2/*`: ADC, RTC, SHTC3, SD, Wi-Fi AP/STA,
  battery/power key, **audio loopback with esp_codec_dev**, LVGL v8/v9, factory
  program, RTC sleep, FT6336) and a **`XiaoZhi` example** with a factory binary
  (`firmware/V2-XiaoZhi-En.bin`) — i.e. the vendor ships an AI-assistant voice
  firmware for this exact board (XiaoZhi-ESP32 style: push-to-talk → server ASR →
  spoken reply).
- **rbustos567/waveshare-esp32-epaper-1.54-voice-client** — ESP-IDF, push-to-talk
  (BOOT), microphone loop, HTTPS `POST` of captured audio to a gateway with device id
  + timestamp headers, 500 ms debounce, `#define GPIO_POWER_LATCH 17` + `set_power(true)`
  at start. Closest published architecture to our note uploader.
- Other Waveshare-listed projects on this board (Resources-And-Documents): InkBoy,
  unidex pocket e-ink OS, Pala One e-reader port, Waveshare e-paper climate display,
  SigBin signature box — display/epaper precedents, not voice.
- **This SDK's own external projects list** (`docs/protocol.md`) includes
  e-paper-adjacent gadgets and two "AI notepad" devices; the protocol has no
  board-specific assumptions beyond `caps`.

---

## 3. Transports

| Transport | Status for this device | Notes |
|---|---|---|
| Wi-Fi 2.4 GHz STA | **Primary.** One WebSocket held to the gateway; reconnect backoff in core; AP-setup page exists (`firmware/esp32/main/wifi_setup.html`, `port_wifi_setup.cpp`). | Live voice, note upload, pairing, OTA all ride it. |
| BLE | **Not in firmware (v1).** `docs/hardware.md` — no gadget board enables Bluetooth yet; `docs/protocol.md` BLE UUIDs are defined but unused. | Do not promise BLE mode. Provisioning uses the Wi-Fi setup AP / browser installer instead. |
| Host-side tailnet (WireGuard) | Optional in the SDK (`profile.tailnet`, `port_tailnet.cpp`) when Hermes is not on the LAN. | Off by default; only relevant if the device leaves this LAN. |
| USB | Console + flash only (S3 native USB Serial/JTAG over the Type-C connector). | Not a runtime transport. |

Consequences for the two product requirements:

1. **Live assistant mode** needs Wi-Fi (or tailnet) and a reachable gateway. With no
   network the button must fail gracefully with an on-screen message, not hang.
2. **Offline notes** record to SD with no network; upload happens when Wi-Fi is back
   *and* the gateway answers. "Bluetooth but no Wi-Fi" is not a mode this firmware
   will have — the note button works offline either way, which is what matters.

---

## 4. Hermes endpoint and protocol

### 4.1 Endpoint

```
ws://<hermes-host>:8765/gadget          subprotocol: hermes-gadget.v1
```

- Default port **8765**, path `/gadget`, `ws` (or `wss` when the gateway is
  configured with TLS). On this host the hub is verified listening on
  `0.0.0.0:8765` (gateway pid on hermes-host, LAN 192.168.1.20) and the existing Office
  gadget connects from 192.168.1.21.
- The authoritative URL comes from **`hermes gadget info`** (prints device URL and
  gadget configuration) or `config.yaml` `platforms.gadget.*`. Changing the port
  needs a gateway restart and a device `server_address` update.
- Config keys the device carries: `server_address`, `access_token` (optional shared
  token — unset on this host), `default_name`. Enrollment/pairing state lives in
  `plugin-data/gadget/devices.json` on the host; device id is
  `hg-<sha256(device_key)[:16]>` (`protocol.py`).

### 4.2 Handshake and identity

Device → server, in order (see `docs/protocol.md` §1):

```jsonc
{"type":"hello","proto":1,"device_id":"hg-<16 hex>","token":"",   // access token if set
 "fw":"0.1.0","nonce":"<128-bit base64>","board":"waveshare-esp32s3-epaper-154"}
{"type":"auth","mac":"base64(HMAC-SHA256(key, "hermes-gadget/v1|" + device_id + "|" + nonce))"}
```

`key` is a 32-byte random device key, minted once, stored in NVS (nvs_flash), base64
in the device config, never sent to the host — the host knows it from enrollment
(`devices.json`) and derives the id from it. Server answers `challenge` (retry with a
new nonce), `paired` (needs `hermes gadget pair`), `rejected` (close), or `welcome`:

```jsonc
{"type":"welcome","accepted":true,"name":"Office","server_time":..., "paired":true,
 "max_utterance_s":60, "transcript":true,
 "profile":{"id":"default","agent":{"stt":"whisper-base-en","tts":"en_US-ryan-high"}}}
```

After `welcome` the connection is heartbeated by the protocol's own JSON ping
(websockets `ping_interval=None` on the hub — do not rely on WS pings).

### 4.3 Framing

- JSON control messages as above (`protocol.message(type=..., ...)` helpers server-side).
- Binary frames: 4-byte header `<BBH>` = `channel, stream, seq` then payload
  (`protocol.BINARY_HEADER`). Channels: `0x01` audio, `0x02` image,
  `0x03` firmware, `0x04` face. A note-upload channel (e.g. `0x05`) is **not**
  needed — notes use HTTP (D3).
- Hub `max_size` is `MAX_FRAME_BYTES`; an audio frame stays at 40 ms
  (24 kHz mono PCM16 → 1920 B payload), which is the pacing the SDK uses.

### 4.4 Messages that matter for our two modes

Device → server: `audio.start {stream, encoding:"pcm16", rate, channels, turn?}`
+ binary frames + `audio.end {stream}`; `text {msg_id, text}`; `cancel {reason}`;
`session.new {reason:"user"}`; `heartbeat`; `battery {pct?, mv, charging, state}`;
`state {sensors}`; `event {event, data, notify_agent}`; `ota.*`; `log.line`,
`console.*` (console lines the board sends to the host; the console itself is read
over USB, see §10.3); `set.name`.

Server → device: `status/transcript/reply/reply.delta/turn.start/turn.end`;
`notice {text, ttl_s, stack}`;
`display.dashboard/display.card/display.choices {text, options[]}`; `say`;
`audio.start {encoding:"pcm16"|"opus", rate, channels, duration_ms, gapless}` +
binary frames (`op 1 = frame`, `op 2 = end`, DTX padding `0xFE/0xFF`);
`system.settings`, `system.battery`, `system.paired`, `system.wifi`,
`ota.offer/ota.data/ota.request/ota.abort`.

Unknown message types and unknown fields are ignored; new message types do not bump
`proto`. So the plugin can add its own device messages later without a version change.

### 4.5 Host-side speech path (already configured here)

- STT/TTS: `voice.speech_server` → `ws://127.0.0.1:8770/v1/realtime`,
  `transcription_model: whisper-base-en`, `voice: en_US-ryan-high` (verified running
  on 127.0.0.1:8770). Optional Local Live front model on 11436 (`voice-agent`).
- Plugin env: `GADGET_ACCESS_TOKEN`, `GADGET_ALLOWED_USERS`, `GADGET_ALLOW_ALL_USERS`,
  `GADGET_HOME_CHANNEL` (unset → pairing codes, which is what we want).
- The device only ever sees its own WebSocket; all ASR/TTS happens on the host.

---

## 5. Audio codec and rates

| Direction | Format | Rate | Frame |
|---|---|---|---|
| mic → Hermes (live turn) | PCM16 mono (`caps.mic.formats:["pcm16"]`) | **24 000 Hz** (`CodecAudio::kRate`) | binary channel `0x01`, 40 ms → 1920 B |
| Hermes → speaker | PCM16 mono (report `["pcm16"]`; libopus optional for both ends) | 24 000 Hz | binary channel `0x01`, op 1, 40 ms frames, `playback_lead_ms` 1000 |
| notes → server (HTTP) | WAV (RIFF/PCM16/mono) | 24 000 Hz | whole file, ≤5 min (14.4 MB) default |

Details:

- `CodecAudio` (ES8311 via `esp_codec_dev`) **only runs at 24 kHz** — `start()` at
  any other rate logs `microphone runs at %u Hz only` and stays at 24 kHz. The
  profile therefore reports `mic_rate = speaker_rate = 24000` (set automatically in
  `main.cpp` when `hal.mic == &g_codec_mic`).
- `max_utterance_s = 60` from `welcome`; 24 kHz × 2 B × 60 s = 2.88 MB held in the
  3 MB `recording_alloc` buffer (16 kHz → 2 MB). Keep the core's default 3 MB.
- Server buffers ~200 ms (`BUFFERED_S`), decodes to 16 kHz mono for STT, samples
  to 0.4 s and drops utterances under 0.25 s.
- Playback pacing is server-side by audio time (`DeviceSession` pump):
  `audio_seconds ≥ 0.5`, ≤ 3.0, `playback_lead_ms` defaults 1000 — do not change.
- Recommended `rate_kbps: 24` only if opus is enabled on both ends; PCM16 over LAN
  costs 96 KB/s ≪ 40 MB/s Wi-Fi capacity, so PCM16 is the default and simplest.
- VAD: `vad_min_rms` / `drop_silent_recordings` come from the codec board config;
  set them from the Waveshare demo's observed noise floor during bring-up **[verify]**.

---

## 6. Buttons and UI behaviour

### 6.1 Button map (port-level; this board has no touch — see the Touch-SKU caveat in §2.1)

| Physical | Logical | Behaviour |
|---|---|---|
| BOOT (GPIO0) | **Talk** | Press-and-hold: start recording (`has_cancel_button` semantics with a long press). Release: send. `talk_key = true`, `wake_key = true` (BOOT is also the deep-sleep wake pin). |
| BOOT held ≥ 2 s | **Cancel** | Port-added long-press → `App::on_button(TouchCancel…)`-equivalent cancel of the current turn / closes overlays (the SDK expects Cancel handling to live in the port). |
| PWR (GPIO18) | **Note record** | Tap: start/stop offline voice note. Vendor firmware treats this pin as a real button (and uses double-press as a gesture), so wire it as `LatchPowerConfig.key` *and* read it in the port; keep the board's own hardware power function (long hold cuts/holds power) untouched. |
| PWR unusable? | fallback | If bring-up shows the latch swallows taps before firmware sees them, fall back to **double-tap BOOT = note** and document the change here. **[verify on hardware — open A/B, evidence favours PWR]** |
| Green LED (GPIO3) | feedback | Solid while a note is recording, slow blink while uploading (port-owned GPIO, no SDK concept needed). |

The 2 s long press arms only when the press found something to cancel — a turn
in flight, or an overlay/question on the screen. A press from the idle screen
starts the recording and holding it is talk for as long as it lasts, so
hold-to-talk keeps working past 2 s (an arming press that started a fresh
recording would cut every utterance at the threshold). Implemented in
`main/talk_hold.hpp` (`hold_cancel_armed` / `TalkHold`), driven from
`port_buttons.cpp`, threshold in `ButtonConfig.talk_hold_cancel_ms`; the state
machine is covered by the host tests in `firmware/tests/test_epaper_live_mode.cpp`,
the gesture itself is a hardware-validation item.

Settings UI, clock, hero/mascot, cards, `display.choices`, auto-paging replies all
come from the core unchanged.

### 6.2 Screen rules for e-paper (this is where an LCD port would mislead you)

- **No animations**: e-paper cannot do 10 fps. The board profile sets
  `DeviceProfile::animation_default = false`, so the `animation` setting starts
  off and every screen transition is one static frame (turning it on is possible,
  and costs a refresh per frame).
- **Redraw budget**: debounce flushes to ≈400 ms, redraw only changed bands, and
  rely on `Display::flush(y0, y1)` bands (200 rows × 200 px × 2 B = 80 KB RGB565
  fits in PSRAM, so `framebuffer_rows() == 0` — whole panel).
- **Refresh strategy inside `EpdDisplay::flush`**: threshold RGB565 → 1 bit,
  partial refresh for incremental changes, **full refresh every K partials (start
  K = 20) and on boot** to clear ghosting; Waveshare also warns the panel must be
  refreshed at least every 24 h while powered (same advice the porting doc gives for
  burn-in) — the doze/clock path already redraws on RTC alarm (`PCF85063` INT,
  GPIO5), which satisfies it.
- **BUSY waits** block: cap a full refresh at ~3 s and report failure through
  `Display::take_redraw_request()` (core redraws) rather than wedging the loop.
- **"Screen off"/doze** must *clear* the panel first (e-paper keeps its image), then
  drop `EPD_PWR` (GPIO6). `set_backlight()` is a no-op; power state is rail state.
- **Layout** 200×200, `font_scale = 1`, `charset: "ascii"`,
  `display.brand = "Waveshare ePaper"`, no QR, ~5–6 `card_items`, `scroll up/down`
  buttons absent → the core's auto-paging (`REPLY_DISPLAY_CHARS`) carries long
  replies; a 4-row `display.choices` page already fits 200 px (else paginate).
- Mascot frames ship as 1-bit RGB565-decoded rasters, so `mascot_px = 200` is a
  natural fit; the UI default hero is fine.

---

## 7. SD card and note file schema

### 7.1 Card

- FAT32, 1-line SDMMC (CLK 39, CMD 41, D0 40), `SDMMC_HOST_SLOT_0`,
  `esp_vfs_fat_sdmmc_mount` at `/sdcard`, `format_if_mount_failed = false` on
  the normal path (never format a user's card), retry mount when absent. The
  one format is explicit: `notes format` unmounts, formats with 16 KiB
  clusters and remounts on a worker task. `SdLogger`'s 30 s retry moved into
  `main/notes/sd_mount.cpp`, which is now the only caller of the mount.
- **Single mount owner**: `SdLogger` and the note store must share one mount (note
  store mounts, `SdLogger` reuses; or expose a tiny `SdMount` helper). Two
  `esp_vfs_fat_sdmmc_mount()` calls on one mount point will fail.
- The existing `sd_log` setting keeps mirroring the device log to
  `/sdcard/hermes.log`; the note tree lives beside it.

### 7.2 Layout

```
/sdcard/
  hermes.log                       # existing SdLogger mirror (unchanged)
  notes/
    pending/<note_id>/
      recording.wav                # RIFF PCM16 mono 24 kHz
      note.json                    # schema below, written last (commit marker)
    sent/<note_id>/                # moved here after HTTP 2xx; kept (prune: >500 entries)
    failed/<note_id>/              # terminal 4xx (bad auth/checksum); shown as "N failed"
```

A note is `pending` only when `note.json` exists — write `recording.wav` to
`recording.wav.part` and rename before writing `note.json`, so a power cut never
leaves a half-note that looks complete. Scan order: directory name ascending
(`note_id` is time-sortable).

### 7.3 `note.json`

```jsonc
{
  "schema_version": 1,
  "note_id": "hg-0123456789abcdef-20261119T204512-0001",  // device_id + UTC + seq
  "device_id": "hg-0123456789abcdef",
  "device_name": "EPaper",
  "firmware": "0.1.0",
  "created_at": 1795140312000,     // unix ms, RTC-backed
  // The wire fields of the upload contract (voice-note-ingestion.md §2): the
  // uploader persists them here verbatim and resends exactly these strings on
  // every retry, so one recording always produces one job_id.
  "recorded_at": "2026-11-19T20:45:12Z",   // ISO-8601 with an offset; raw MAC input
  // Before the first clock sync the board's time is 1970: a note recorded then
  // is uploaded stamped with the time it is sent (and that string is written
  // back here), never with 1970 — see the paragraph below.
  "duration_seconds": 8,                   // 1..900, advisory
  "idempotency_key": "hg-0123456789abcdef-20261119T204512-0001",  // = note_id
  "sample_rate": 24000,
  "channels": 1,
  "encoding": "wav_pcm16",
  "bytes": 394144,
  "sha256": "<hex of recording.wav>",      // sent as `checksum` and in the MAC
  "attempts": 0,                    // bumped by the uploader, informational
  "battery_mv": 4012,               // optional, from ADC1_CH3 ×2
  "priority": "normal"              // reserved, Pika-style levels are a future idea
}
```

The wire idempotency key is **`note_id`**, sent as the `idempotency_key` part:
it is generated once per recording, fits the contract's key charset, and the
server derives `job_id = sha256hex(device_id|note_id)` from it
(voice-note-ingestion.md §5.1). The device retries the same bytes and the
same field strings until it gets a 2xx — `202 accepted` (also with
`requeued: true` when a quarantined job was reopened) or `200 duplicate` move
the folder to `sent/`; a `4xx` moves it to `failed/` and stops; `5xx`, a
timeout or no answer at all keep it `pending/` under the uploader's backoff
(5 s ×2, capped at 300 s — §3.3 of the contract). Reboot, battery pull, card
yank and Wi-Fi flaps therefore converge to "uploaded exactly once".

**A note with no clock.** This board has no RTC: it starts at 1970 and only
learns the time from the network. Since `port_time.cpp` treats an unset clock as
a demand of its own, SNTP starts as soon as Wi-Fi is up — with the clock face
still off — so a note recorded after the join carries its real capture time
(`diag.time` is valid within seconds of the connection). A note recorded before
that first sync, or while Wi-Fi is down, would otherwise carry
`1970-01-01T00:2x:xxZ` all the way into the vault, where the filename is built
from `recorded_at` (voice-note-ingestion.md §5.5). As a fallback
`notes::stamped_for_upload` replaces a stamp earlier than 2024-01-01 with the
time the note is finally sent, and `begin_sync` writes it back to `note.json` —
so the retries still resend one stable set of strings, and the vault note is
dated when it arrived. `note_id` and the `idempotency_key` are untouched (the
job comes from the key), so a re-stamp cannot fork a recording into two jobs; a
plausible stamp is never rewritten. `notes status` counts the pending notes
whose id still says 1970 (`… | 2 undated`); the count drops as each one is
uploaded.

---

## 8. Note upload contract (firmware ⇄ Hermes)

**Endpoint** (D3): HTTP inside the gadget plugin, same host as the hub:

```
POST http://<hermes-host>:8766/notes/v1
```

- Served by the **gadget plugin's adapter in the gateway process**: an
  `uvicorn`/`fastapi` app (both ship with Hermes — `plugin/dashboard/plugin_api.py`
  already imports fastapi) started when the platform is enabled, stopped with the
  gateway. Port from `platforms.gadget.extra.notes_port` (default 8766; 0 disables).
  8766 is free on this host today.
- Why not the hub port: `websockets` 15.0.1 `process_request` (the hub's
  `_check_request`) sees request headers only, and reading an HTTP body there is not
  part of the API — folding `/notes/v1` into 8765 would need a hand-rolled body read
  for a 14 MB payload. Why not `plugin/dashboard/plugin_api.py`: it runs in the
  *dashboard* process, behind dashboard auth, and never reaches the gateway's device
  state.

**Request** (normative: [the voice-note upload contract](voice-note-ingestion.md) §2;
this is the firmware's view of it)

One `multipart/form-data` POST. The field values are the strings persisted in
the note's `note.json` (§7.3) — resent byte-identical on every retry; only the
multipart boundary may change.

```http
POST /notes/v1 HTTP/1.1
Content-Type: multipart/form-data; boundary=----hg-note
X-Gadget-Ts: 1795140312            # unix seconds (PCF85063 keeps real time)
X-Gadget-Mac: base64(HMAC-SHA256(device_key, "hermes-gadget/v1|note|" +
    device_id + "|" + ts + "|" + recorded_at + "|" + duration_seconds + "|" +
    sha256hex + "|" + idempotency_key))

------hg-note
Content-Disposition: form-data; name="audio"; filename="recording.wav"
Content-Type: audio/wav

<the WAV bytes>
------hg-note
Content-Disposition: form-data; name="device_id"

hg-0123456789abcdef
------hg-note
Content-Disposition: form-data; name="recorded_at"

2026-11-19T20:45:12Z
------hg-note
Content-Disposition: form-data; name="duration_seconds"

8
------hg-note
Content-Disposition: form-data; name="idempotency_key"

hg-0123456789abcdef-20261119T204512-0001
------hg-note
Content-Disposition: form-data; name="checksum"

<64 hex of the WAV bytes>
------hg-note--
```

Same key, same HMAC shape as the WS `auth` (`protocol.note_mac` on the host,
`hg::crypto` on the device; new context string `hermes-gadget/v1|note|…`), so
enrollment and key storage are reused as-is. Timestamp skew window ±300 s (the
RTC makes it real). Optional `GADGET_ACCESS_TOKEN` goes in `X-Gadget-Token`.
The declared part type and filename are advisory — the server sniffs the
bytes — and the codec is WAV PCM16 mono as recorded (the server also accepts
Ogg Opus for boards that record it, contract §2.3).

**Firmware example** — building the same request (crypto from
`firmware/core/include/hg/crypto.hpp`, transport `esp_http_client`):

```cpp
// The field values come from note.json (§7.3): resend them verbatim.
std::string canonical = "hermes-gadget/v1|note|" + device_id + "|" + ts + "|" +
                        recorded_at + "|" + duration_seconds + "|" + sha_hex +
                        "|" + note_id;  // idempotency_key = note_id
auto digest = hg::crypto::hmac_sha256(key, key_len,
    reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
std::string mac = hg::crypto::base64_encode(digest.data(), digest.size());

// One body: a file part for the audio, text parts for the fields.
std::string body;
auto field = [&](const char* name, const std::string& value) {
  body += "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"" +
          name + "\"\r\n\r\n" + value + "\r\n";
};
field("device_id", device_id);
field("recorded_at", recorded_at);
field("duration_seconds", duration_seconds);
field("idempotency_key", note_id);
field("checksum", sha_hex);
body += "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"audio\"; "
        "filename=\"recording.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
body.append(audio.begin(), audio.end());   // the bytes from the SD card
body += "\r\n--" BOUNDARY "--\r\n";

esp_http_client_handle_t client = esp_http_client_init(&cfg);  // cfg.url = url
esp_http_client_set_method(client, HTTP_METHOD_POST);
esp_http_client_set_header(client, "Content-Type",
                           "multipart/form-data; boundary=" BOUNDARY);
esp_http_client_set_header(client, "X-Gadget-Ts", ts.c_str());
esp_http_client_set_header(client, "X-Gadget-Mac", mac.c_str());
esp_http_client_set_post_field(client, body.data(), body.size());
esp_http_client_perform(client);
// 202 / 200  -> pending/ becomes sent/
// 4xx        -> failed/, never resent (see the response table)
// 5xx / no response -> keep pending/, back off and resend the same bytes
//                      (contract §3.3: 5 s ×2, capped at 300 s)
```

**Responses** (envelope and full status table: contract §3)

| Code | Body | Device action |
|---|---|---|
| 202 | `{"status":"accepted","job_id":…}` (+ `requeued:true` when a quarantined job was reopened) | move `pending/` → `sent/`, count as synced |
| 200 | `{"status":"duplicate","job_id":…}` | same as accepted |
| 401 | `{"error":{"code":"auth", …, "retryable":false}}` | → `failed/`, stop retrying (bad key/pairing) |
| 409 | `checksum_mismatch` (corrupt transfer) or `idempotency_conflict` (same key, different audio) | → `failed/` |
| 400 / 404 / 405 / 413 / 415 / 422 | `{"error":{"code":…, "field"?:…, "retryable":false}}` | → `failed/` |
| 500 / 503 / 507 | `{"error":{"code":…, "retryable":true}}` | retry with backoff 5 s → 10 s → … → 5 min, `attempts++` |
| no response (timeout/reset/refused) | — | retry as above |

Size cap 32 MB for the audio (32 MB + 64 KiB for the whole request, checked
from `Content-Length` before the body is read). The server reads and verifies
the full body first, then writes it under
`~/.hermes/plugin-data/gadget/notes/.staging/<job_id>-*` (fsync, `recording.<ext>`
+ `note.json`) and renames the directory into `inbox/<job_id>/` — an entry
appears complete or not at all, so a device that disconnects mid-body leaves
nothing for the pipeline to process.

**Server pipeline** (implemented in `plugin/notes_pipeline.py`; the runbook,
including a `curl` upload and troubleshooting, is
[`plugin/NOTES.md`](../plugin/NOTES.md)): inbox → STT (host transcription,
same provider family the gadget voice path uses) → summarize with the
profile's model → write markdown to **`~/obsidian/Notes/voice notes/`** as
`YYYY-MM-DD_HHMM_<device-id>_<short-id>.md` with frontmatter `idempotency_key`
(dedupe: a note that already carries this key closes the job without a second
write), copy the audio byte-identical to `voice notes/_audio/<same stem>.<ext>`,
then move the inbox entry to `done/`. Response 202 is sent as soon as the file
is durably stored, *before* STT — the device must not wait on a model
round-trip; pipeline failures are host-side retries (`min(30 s × 2^(n−1), 3600 s)`
over 8 attempts, then `failed/` with `.error`), never device retries.

---

## 9. Power and charging

| Item | Value | Source |
|---|---|---|
| Charger | ETA6098 (MX1.25 battery in, VIN 5 V) | schematic |
| Charge current | ISET = 820 kΩ → **≈200 mA** per the ETA6098 table (820k→0.2 A, 360k→0.5 A … 82k→2 A) — **[verify which resistor is ISET on the populated board]** | schematic + datasheet table |
| VBAT latch | GPIO17 high at boot (`LatchPowerConfig.enable`, pattern in `port_latch_power.cpp` — set level *before* configuring the pin) | vendor demo `VBAT_PWR_PIN` |
| Battery read | ADC1_CH3 = GPIO4, `VBAT = mV × 2`, `ADC_ATTEN_DB_12`, curve-fitting calibration | vendor `01_ADC_Test` |
| Power key | GPIO18 (`LatchPowerConfig.key`, active low with pull-up) | vendor `07_BATT_PWR_Test` |
| Panel / audio rails | GPIO6 (EPD), GPIO42 (codec) — gate both in `board.cpp` before first use | `user_config.h` |
| Charger status | ETA6098 `STAT` → charge/charged indication. Whether it reaches a free GPIO is **[verify]** — if not, report `charging: unknown` and just show the ADC trend | schematic |
| USB power detect | via S3 `USB_SERIAL_JTAG` connection state (works with the USB console) — the SDK's `external_power()`/`on_battery` wiring (`g_stopwatch.external_power()`) is the existing seam | `main.cpp` |
| Power-up order (vendor) | power init → **EPD rail → audio rail → VBAT latch** → buttons (wait for PWR release) → SD → ADC → SHTC3 → codec init; mirror this order in `board.cpp` | vendor `11_FactoryProgram` source |
| Deep sleep | 2 µA (panel retained, controller powered down) + ESP32-S3 deep sleep; RTC alarm (PCF85063, GPIO5) or BOOT (GPIO0) wakes | datasheet, vendor `12_RTC_Sleep_Test` |
| Light sleep / doze | Supported (`CONFIG_PM_ENABLE`, tickless idle, `EspSystem::enable_doze`) — copy the LCD-1.54 board's sdkconfig set, including `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` so the USB console keeps working | `boards/waveshare-esp32s3-lcd-154/sdkconfig.defaults` |
| Panel energy | ~8 mAs per update (≈22 µA·h) — negligible next to Wi-Fi; **doze cost is Wi-Fi**, so keep the core's doze rules and let the RTC alarm drive clock refreshes | datasheet §2.4 |
| Battery percentage | No fuel gauge: report millivolts only; percentage needs `set battery_estimate liion` (Vbatt curve, opt-in) and is then an estimate | `docs/hardware.md` battery section |

Practical budget (target, **[verify]**): ~45–60 min continuous voice, ~8 h doze with
an hourly clock refresh, weeks on the shelf in deep sleep. The firmware must never
assume a stable rail: gate EPD/audio power around use, and latch VBAT before anything
else at boot.

---

## 10. Toolchain, build and USB flash procedure

### 10.1 Toolchain (exact)

- **Framework choice: ESP-IDF via this repo** (the vendor supports Arduino IDE *and*
  ESP-IDF — <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/Arduino> and
  <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/ESP-IDF> — and the pin map above
  was read out of the vendor's ESP-IDF examples). The gadget SDK is an IDF project,
  so the Arduino route would mean rewriting the runtime; nothing is borrowed from the
  Arduino demo.
- **ESP-IDF** — `firmware/esp32` is an IDF project; this repo's PlatformIO wrapper
  pins `platform = espressif32`, `framework = espidf` (a recent CI build resolved
  **platform 8.0.0 / ESP-IDF 6.1.0** — pin what the build resolves and record it).
  Browser installer (release manifests) uses `esptool-js` ≥ 0.10.1.
- Python deps: `python -m pip install -e ".[dev]"` (repo `pyproject.toml`).
- Optional for host-side sim: `hermes-gadget sim --live-audio` (2 Mbps virtual
  device over 127.0.0.1) — good for protocol work without hardware.
- On this host: use the Hermes toolchain python
  (`~/.hermes/tools/python-3.14.9*/bin/python3.14`) and remember `PYTHONPATH=`
  before invoking a venv's pip (exported globally here).

### 10.2 Build

```bash
cd hermes-gadget-sdk
python -m pip install -e ".[dev]"
cd firmware/esp32
pio run -e waveshare-epaper-154                 # or:
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-epaper-154/sdkconfig.defaults" build
```

### 10.3 Flash (USB-C)

1. **Back up the factory image first** (holds Wi-Fi credentials and the vendor demo):

   ```bash
   esptool.py --chip esp32s3 -b 921600 read_flash 0 0x800000 factory-epaper154-v2.bin
   ```

   (8 MB = `0x800000`; the `firmware/V2-FactoryProgram*.bin` in the vendor repo is
   the other restore path.)
2. Connect the board's Type-C to the PC. v2 exposes the S3's **native USB
   Serial/JTAG** (schematic `U_N/U_P` → Type-C), so the port is `/dev/ttyACM*`.
   If it is not enumerated: hold **BOOT (GPIO0)**, tap **EN/RESET** (R2/EN on the
   schematic — the PWR key or plugging in while BOOT is held also works), release
   BOOT → USB-JTAG/serial download mode.
3. Flash:

   ```bash
   pio run -e waveshare-epaper-154 -t upload      # or:
   idf.py -D SDKCONFIG_DEFAULTS="…" flash monitor # 115200 baud console
   ```

4. Console/monitor: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` (the Type-C **is** the
   console on this board, same situation as the Waveshare LCD-1.54), 115200. Watch it
   with `hermes-gadget console --port /dev/ttyACM0`, `idf.py monitor` or
   `pio device monitor`; once the board is paired, `hermes gadget diag` saves the same
   diagnostics report over the network, without the cable.
5. **Toolchain path**: PlatformIO's install/flash of esptool and the IDF toolchain
   downloads at first build (internet needed once). Checksums/versions are what
   PlatformIO resolves — record them in the flash log of the validation task.
6. OTA afterwards: `hermes gadget update` (`ota.offer` over the open socket; app
   image slot 1.9 MB — `tools/check_size.py` fails the build under 10 % free;
   `caps.ota.max_size` reported by the device; 5-min probation rollback requires the
   new image to reach Hermes).

### 10.4 Pairing

First boot shows a pairing code (device id `hg-…`); on the host:

```bash
hermes -p <profile-with-gadget> gadget pair      # or: hermes pairing approve gadget <CODE>
```

`hermes gadget info` prints the device URL (`ws://192.168.1.20:8765/gadget` on this
host). A USB-enumerated board is a serial port (`/dev/ttyACM0` on Linux, `COM5` on
Windows; `pio device list` shows it), and that port is what `hermes-gadget console`,
`provision` and `diag` take as `--port`.

---

## 11. Proposed firmware module breakdown

Paths are relative to the repo root (`hermes-gadget-sdk`). ★ =
new, ≈ = modified, ○ = existing (no change expected).

### Board definition (follows `docs/porting.md` exactly)

| File | Change | Contents |
|---|---|---|
| `firmware/esp32/boards/waveshare-epaper-154/board.json` | ★ | title/summary/`docs`/`ready_made` (copy `waveshare-esp32s3-lcd-154/board.json` shape) |
| `firmware/esp32/boards/waveshare-epaper-154/sdkconfig.defaults` | ★ | `CONFIG_IDF_TARGET="esp32s3"`, `FLASHSIZE_8MB` (not 4MB/16MB), `SPIRAM=y/MODE_OCT/SPEED_80M/SPIRAM_USE_MALLOC/SPIRAM_IGNORE_NOTFOUND`, `ESP_DEFAULT_CPU_FREQ_MHZ_240`, `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` (unset UART default), `CONFIG_HG_BOARD_WS_EPAPER_154=y`, `PM_ENABLE`, `FREERTOS_USE_TICKLESS_IDLE`, `USJ_NO_AUTO_LS_ON_CONNECTION`, `RTC_CLK_SRC_INT_8MD256`, `PM_LIGHT_SLEEP_CALLBACKS`, `LWIP_TCP_WND/SND_BUF 32768`, `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` — all lifted from the LCD-1.54 file because the electrical situation (octal PSRAM, USB console, doze) is identical |
| `firmware/esp32/main/Kconfig.projbuild` | ≈ | add `config HG_BOARD_WS_EPAPER_154 bool "Waveshare ESP32-S3-ePaper-1.54 (200x200 e-paper + ES8311)"` to the `choice HG_BOARD` block (upstream-visible, tiny) |
| `firmware/esp32/main/board.cpp` | ≈ | `#elif CONFIG_HG_BOARD_WS_EPAPER_154 → HG_BOARD_NAME "waveshare-epaper-154"` + `make()`: `epd` config (SPI 12/13, cs 11, dc 10, rst 9, busy 8, rail 6, 200×200), `codec` ES8311 speaker **and** mic (`MicCodec::Es8311`, i2c 47/48, `pa` 46, i2s mclk 14/bclk 15/ws 38/dout 45/din 16, `audio rail 42` gated before `g_codec.begin`), `buttons {talk: 0, cancel: -1, …}` + `LatchPowerConfig{enable:17, adc:4, ratio:2, key:18}`, `SdConfig{enabled:true, clk:39, cmd:41, d0:40}`, i2c bus 47/48, no touch/encoder/camera |
| `firmware/esp32/main/board.hpp` | ≈ | add `struct EpdConfig { bool enabled; int sclk, mosi, cs, dc, rst, busy, pwr; uint16_t width, height; }` next to `LcdConfig` (+ the `EpdConfig epd{}` member) |
| `firmware/esp32/platformio.ini` | ≈ | `[env:waveshare-epaper-154]` — `board = esp32-s3-devkitc-1`, `board_build.flash_size = 8MB`, `cmake_extra_args` with the board's `sdkconfig.defaults` |

### Display

| File | Change | Contents |
|---|---|---|
| `firmware/esp32/main/port_display_epd.hpp/.cpp` | ★ | `class EpdDisplay : public hg::Display` — owns the SPI device, the 80 KB RGB565 framebuffer in PSRAM, and the vendor command sequence (init, full LUT, partial LUT, `EPD_SetWindows/EPD_SetCursor`, BUSY poll with 3 s cap). `flush(y0,y1)` thresholds RGB565→1 bit, partial-refreshes by default, full-refreshes every K partials/boot/failure, drives `EPD_PWR` (GPIO6), implements `take_redraw_request()` on BUSY timeout, `set_backlight()` no-op. `info()` → 200×200, `rgb565`, rows whole panel. Reference implementation: vendor `epaper_driver_bsp.cpp` (sparse checkout noted in §12) |
| `firmware/esp32/main/main.cpp` | ≈ | select `EpdDisplay` in the display block (mirrors the `LcdConfig` branch); no panel controller enum needed if `board.epd.enabled` |

### Audio, power, system

| File | Change | Contents |
|---|---|---|
| `port_codec.cpp` / `CodecAudioConfig` | ○ / ≈ | ES8311 speaker + `MicCodec::Es8311` already supported; only the audio-rail gate (GPIO42) before `begin()` is ours — small `#if`/config hook |
| `firmware/esp32/main/port_power*.cpp`, `port_latch_power.cpp` | ○ | reuse `LatchPowerConfig` for GPIO17/ADC4/ratio 2/key 18 as-is |
| `firmware/esp32/main/port_sdlog.cpp` | ≈ | expose the mount (or accept an external one) so the note store shares it; behaviour otherwise unchanged |

### Offline voice notes (the one feature the SDK does not have)

| File | Change | Contents |
|---|---|---|
| `firmware/esp32/main/notes/notes_store.hpp/.cpp` | ★ | SD layout of §7: scan/mkdir, atomic `.part`+rename, `note.json` read/write, pending→sent/failed moves, ordered delete (`erase`), prune rule, card-absent tolerance |
| `firmware/esp32/main/notes/notes_queue.hpp/.cpp` | ★ | note ids, `note.json` schema, SHA-256, the multipart request and `canonical_string`, response classification, retry backoff |
| `firmware/esp32/main/notes/notes_capture.hpp/.cpp` | ★ | microphone → `recording.wav`: header written first and patched at stop, the duration/byte caps, a clean abort |
| `firmware/esp32/main/notes/notes_uploader.hpp/.cpp` | ★ | walks `pending/`, signs headers per §8, `esp_http_client` POST to `:8766/notes/v1`, sliced per loop pass, honours `accepted`/`duplicate`/`4xx`, updates `note.json.attempts` |
| `firmware/esp32/main/notes/sd_mount.hpp/.cpp` | ★ | the single mount owner the `SdLogger` and the note store share; `notes format` runs the one explicit format |
| `firmware/esp32/main/modes/notes_mode.hpp/.cpp` | ★ | the mode itself: PWR tap → record/stop, the status line, the sync clock, the `notes` console command |
| `firmware/esp32/main/main.cpp` | ≈ | `NotesModeImpl` with the board's mic, battery and card; the `notes`/`screen` console lines |
| `firmware/core/include/hg/app.hpp` + `core/src/app.cpp` | ≈ *(additive only)* | `App::on_mic_samples_extra` (a mode's own copy of every mic buffer), `App::on_console` (a board module's own console lines), `App::access_token()`/`App::sign()` (the module signs its own request with the core's key) |
| `firmware/esp32/main/ui_state.hpp/.cpp` | ≈ | `UiState::set_status()` — a mode's own line, shown while the phase is Idle, republished only when it changes or its TTL runs low |

### Host side (this repo's `plugin/`)

| File | Change | Contents |
|---|---|---|
| `plugin/notes_ingest.py` | ★ | the multipart upload endpoint on `platforms.gadget.extra.notes_port` (8766): six-part parse, fixed validation order (400 → 413 → 422 → 415 → 401 → 409 → idempotency), HMAC/ts/checksum via `protocol.note_mac`, durable `.staging/<job_id>-*` → `inbox/<job_id>/`, duplicate/conflict/requeue ledger, 202-before-STT |
| `plugin/notes_pipeline.py` | ★ | inbox → STT → summarize → `~/obsidian/Notes/voice notes/` markdown (`YYYY-MM-DD_HHMM_<device-id>_<short-id>.md`) + `_audio/` copy, dedupe on frontmatter `idempotency_key`, `min(30 s × 2^(n−1), 3600 s)` × 8 attempts then `failed/`, then `done/` |
| `plugin/adapter.py` | ≈ | start/stop the notes server with the platform; expose pending counts if the device message path (§11 core change) is taken |
| `plugin/protocol.py` | ≈ | add `NOTE_CONTEXT = b"hermes-gadget/v1|note|"` + `note_mac()` helper (same shape as `auth_mac`) |
| `plugin/NOTES.md` | ★ | the endpoint's API reference and runbook: request fields, auth, the `curl` example, response codes, idempotency, vault layout, retry/backoff, troubleshooting, and the test commands (contract tests + `tests/test_notes_e2e.py`) |

### Not in scope here

BLE provisioning, a native phone companion (the relay page covers the phone side), Opus capture
on this board (the server already accepts Ogg Opus for boards that record it —
contract §2.3), multi-device note routing rules, and any change to the vendor's
demo code we copy — its license headers stay intact under `LICENSES/`.

---

## 12. Open questions to settle on hardware

1. **Refresh timing**: measured full vs partial refresh on this panel (vendor driver
   polls BUSY with 5 ms sleeps; no published number in the datasheet text). Drives the
   debounce/K-partial constants.
2. **PA/rail semantics**: GPIO42 as audio rail + GPIO46 as `pa` (demo config) vs the
   schematic's `PA_EN`/`PA_CTRL` labels — confirm audio comes up and set
   `speaker_pa` accordingly.
3. **PWR tap as the note button** (vs BOOT double-tap fallback) — the vendor
   firmware reads GPIO18 as a button and uses double-press as a gesture, so the
   evidence favours PWR; confirm the power latch does not swallow taps first.
4. **Charger status GPIO**: reachable or ADC-trend only.
5. **SD capacity/format variance**: test a real card (FAT32 assumed; mount retries,
   never formats).
6. **VAD thresholds** for `vad_min_rms` / `drop_silent_recordings` on the ES8311 mic.
7. Whether the notes HTTP endpoint stays on its own port (8766) or gets folded into
   the hub's 8765 if a later websockets API makes body reading easy.
8. **Which SKU is on the desk** — non-touch `ESP32-S3-ePaper-1.54` (this spec) or
   `ESP32-S3-Touch-ePaper-1.54` (FT6336 + `EPD_TP_RST` on the shared I2C bus). One
   glance at the bezel decides it; if touch, §6 gains tap-to-talk/tap-to-note and
   `BoardConfig.touch_screen` becomes true.

---

## 13. References

Vendor:

- Product/docs index — <https://docs.waveshare.com/ESP32-S3-ePaper-1.54>
- Resources, demos, schematics, community projects —
  <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/Resources-And-Documents>
- Arduino demo — <https://docs.waveshare.com/ESP32-S3-ePaper-1.54/Arduino>
- Demo source (pin map, audio loopback, SD, power-key, RTC sleep) —
  <https://github.com/waveshareteam/ESP32-S3-ePaper-1.54>
- Schematic PDF (v2 / touch variant, same core nets) —
  <https://github.com/waveshareteam/ESP32-S3-ePaper-1.54/raw/main/04_Hardware/Schematics/ESP32-S3-Touch-ePaper-1.54-Schematic.pdf>
- Panel datasheet (1.54″ e-Paper V2, power figures) —
  <https://files.waveshare.com/wiki/common/1.54inch_e-paper_V2_Datasheet.pdf>

This repo (all read at `da48d2570553ff3a8090e32b0eab0c9eb71a7684`):

- `docs/porting.md` (board checklists, display/audio/power/seams, sizing)
- `docs/protocol.md` (normative wire protocol — identity, auth, messages, framing)
- `docs/hardware.md`, `docs/supported-hardware.md` (no e-paper board today; display
  matrix has no e-paper entry)
- `docs/setup-board.md` (installer, esptool, `read_flash` backup, USB troubleshooting)
- `docs/connect-hermes.md` (plugin install, `hermes gadget info`, pairing)
- `docs/architecture.md` (Display band framebuffer, speech path)
- `firmware/core/include/hg/hal.hpp` (`Display`, `AudioIn` interfaces),
  `firmware/core/include/hg/app.hpp` (`DeviceProfile`, `emit_event`),
  `firmware/esp32/main/board.hpp` (`LcdConfig`, `CodecAudioConfig`,
  `SdConfig`, `LatchPowerConfig`, `ButtonConfig`),
  `firmware/esp32/main/port_latch_power.cpp`, `firmware/esp32/main/port_sdlog.cpp`,
  `firmware/esp32/boards/waveshare-esp32s3-lcd-154/sdkconfig.defaults`
- `firmware/esp32/boards/*` (38 board ports), `firmware/esp32/platformio.ini`

Installed on this host:

- `~/.hermes/plugins/gadget/` (plugin 0.6.0, `protocol.py`, `hub.py`,
  `dashboard/plugin_api.py` — note: the install has **no `.git`** and
  `plugin-update-checks` flags "provenance drift"; reinstall or link from this fork)
- `~/.hermes/plugin-data/gadget/devices.json` (Office device
  `hg-0123456789abcdef`, board `esp32-s3-box`, fw 0.6.0)
- `~/.hermes/config.yaml` (`platforms.gadget.enabled`, home channel "Office",
  `voice.speech_server ws://127.0.0.1:8770/v1/realtime`)
- Live listeners verified on hermes-host: `0.0.0.0:8765` (gadget hub), `127.0.0.1:8770`
  (speech), `127.0.0.1:11436` (local-live model); 8766 free

Community:

- Pika Note (same board, Android BLE companion, Google-Drive transcript backend) —
  <https://github.com/zaera/Pika_note>
- Voice Commander client (ESP-IDF push-to-talk + HTTPS audio POST + GPIO17 latch) —
  <https://github.com/rbustos567/waveshare-esp32-epaper-1.54-voice-client>
- Waveshare XiaoZhi factory AI firmware (`02_Example/XiaoZhi`,
  `firmware/V2-XiaoZhi-En.bin`)
- Waveshare community project gallery (Resources-And-Documents) — Pala One voice
  notes, InkBoy, unidex, SigBin
