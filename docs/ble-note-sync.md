# Bluetooth note sync

The e-paper note device records with no network at all, and its notes normally
leave over Wi-Fi (`POST /notes/v1`). This is the other way out: a phone takes the
note off the card over Bluetooth LE, with no Wi-Fi anywhere, and posts it to
Hermes itself.

It is one GATT service beside the [phone app channel](protocol.md#phone-app-over-bluetooth)
and shares its pairing. A note moves out of `pending/` only when the phone says
Hermes has it, so a transfer can be interrupted at any byte and resumed.

| | |
|---|---|
| Device side | `firmware/esp32/main/notes/notes_ble.{hpp,cpp}` (protocol), `port_ble.cpp` (radio), `modes/notes_mode.cpp` (the card) |
| Phone side | `web/ble-note-relay/` (Web Bluetooth page + protocol + tests) |
| Tests | `firmware/tests/test_epaper_ble_sync.cpp` (16 cases), `web/ble-note-relay/selftest.mjs` (11 cases, on the firmware's own packet dump) |

## 1. The service

| | UUID | Properties |
|---|---|---|
| Service | `dc5300a0-7a2a-400f-b8f6-e87185576192` | Primary |
| Control | `dc5300a1-7a2a-400f-b8f6-e87185576192` | Write with response; encrypted and authenticated link required |
| Data | `dc5300a2-7a2a-400f-b8f6-e87185576192` | Notify; subscribe before the first `get` |

Both characteristics need the same paired phone as the console channel: the
device only advertises while **Phone app** pairing is open or a phone is already
paired, and it hands out a six-digit passkey that the phone's user types
([pairing](protocol.md#phone-app-over-bluetooth)). A note is private; an
unencrypted link would let anything in radio range read it.

The link must negotiate an ATT MTU of at least **64** bytes: a command has to fit
one write. iOS negotiates about 185; the device asks for 247
(`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`).

## 2. Commands (control characteristic)

One UTF-8 line per write, no terminator needed (`\r\n` is tolerated). Answers
come back on the data characteristic.

| Command | Answer |
|---|---|
| `hello` | `ok ble-note-sync/1 mtu <n>` |
| `list` | `L` lines: the count, then one line per pending note, then `END` |
| `get <note_id>` | the note's header, then its blocks, then the end packet |
| `get next` | the same for the **oldest** pending note, or `ok nothing pending` |
| `resume <note_id> <offset>` | the same as `get`, continuing at `offset` |
| `ack <offset>` | nothing: it releases the next window |
| `done <note_id>` | `ok synced <note_id>`; the note moves `pending/` → `sent/` |
| `stop` | `ok stopped`; the note stays pending |

A list line is `<note_id> <bytes> <duration_s> <sha256|-> <recorded_at>`, the
same fields the header repeats. `<sha256>` is `-` when the note has not been
hashed yet (a note is hashed before its first Wi-Fi upload).

```
→ hello
← L "ok ble-note-sync/1 mtu 185"
→ list
← L "1"
← L "epaper-20261120T081504-0001 144044 30 3f9c… 2026-11-20T08:15:04Z"
← L "END"
→ get next
← H "epaper-20261120T081504-0001 144044 30 3f9c… 2026-11-20T08:15:04Z"
← B offset=0 crc32=… 171 bytes
…                (16 blocks, then it waits)
→ ack 2736
← B offset=2736 … (the next window)
…
← K
→ done epaper-20261120T081504-0001
← L "ok synced epaper-20261120T081504-0001"
```

## 3. Packets (data characteristic)

One packet per notification, never larger than `MTU - 3` bytes. The first byte
is the tag.

| Tag | Payload |
|---|---|
| `H` `0x48` | a fragment of the header line, UTF-8, no newline |
| `B` `0x42` | one audio block: `offset` u32 LE, `crc32` u32 LE, `length` u16 LE, then that many bytes |
| `L` `0x4c` | a fragment of an answer line; the line ends at `\n` |
| `E` `0x45` | a fragment of an error line; the line ends at `\n` |
| `K` `0x4b` | the last packet of a note: every byte is out and acknowledged |

Text answers (`L`, `E`, "header") are **fragments**: a phone concatenates
payloads of the same tag until it sees `\n`. That is what keeps a 150-byte header
line inside a 20-byte notification on a link that never grew its MTU.

A block's payload is `min(MTU - 3 - 11, 512)` bytes, so about 171 bytes on iOS.
`crc32` is the IEEE 802.3 CRC-32 (zlib's) of that payload, computed on the device
— the phone must check it before writing the bytes into its buffer.

## 4. The rules

- **Windows.** The device notifies at most 16 blocks, then stops. `ack <offset>`
  says "I have everything before this byte offset", and the next window follows.
  A phone may acknowledge after any block; it must not acknowledge past what it
  has stored.
- **Resumption.** `offset` is absolute, so a lower `ack` makes the device rewind
  and resend, and a dropped link is resumed with `resume <id> <offset>`. The
  device never trusts its own position more than the phone's.
- **Integrity.** Each block carries its own CRC-32; the header carries the WAV's
  SHA-256. On a note that has not been hashed yet (`-`), the phone still checks
  every block, and the upload's `checksum` part is the digest the phone computed.
- **Nothing is written during a transfer.** The note stays in `pending/` until
  `done`, which the phone sends only after Hermes accepted the upload. A note in
  `failed/` is never offered (it needs a person); a note in `sent/` is done.
- **Timeouts.** 15 s without an `ack` ends the transfer with
  `E the phone stopped acknowledging`; 60 s without `done` after the end packet
  ends it with `E the phone did not confirm the note`. Both leave the note
  pending, so nothing is lost.
- **One at a time.** `get` for a note while another transfer runs replaces it. A
  `done` for the note in flight closes the transfer.

## 5. Failure handling

| What goes wrong | What happens |
|---|---|
| Bluetooth drops mid-transfer | The device closes the file, forgets the transfer, keeps the note. Log: `phone disconnected`. The phone resumes from the last offset it stored. |
| The phone misses a block | Its `ack` is lower than the device's position: the device seeks back and resends from there. |
| The phone stops answering | After 15 s: `E the phone stopped acknowledging`, the screen returns to the counts, the note stays pending. |
| The phone never confirms | After 60 s: `E the phone did not confirm the note`; the note stays pending and can be offered again. |
| The note is empty (0 bytes) | `E empty note <id>`; nothing is transferred, and `list` shows it with `0` bytes so the phone can skip it. |
| No card | `E no card` for `list`, `get` and `get next`; nothing else changes. |
| Unknown or non-pending id | `E unknown note <id>` (a note in `sent/` or `failed/` is not offered). |
| Offset past the end | `E offset past the end of <id>`. |
| MTU below 64 | `E Bluetooth MTU too small` for every command; the phone should reconnect and ask for a bigger MTU. |
| Phone not paired | The write is refused at the ATT layer (`insufficient authentication`); nothing reaches the app. |
| Phone not subscribed to Data | Nothing is notified; the transfer stalls and ends with the 15 s timeout. |
| A second phone | The device serves one connection at a time; the second is refused while the first is connected. |

## 6. Speed

A 24 kHz PCM16 mono note is 48 KB per second of audio, which is the reason this
channel is for notes that do not need to be anywhere fast.

| Note | Bytes | ≈ at 10 KB/s | ≈ at 25 KB/s | ≈ at 50 KB/s |
|---|---|---|---|---|
| 10 s | 480 KB | 48 s | 19 s | 10 s |
| 30 s | 1.4 MB | 2.3 min | 56 s | 28 s |
| 300 s (cap) | 14 MB | 23 min | 9 min | 4.7 min |

Real throughput on iOS lands around 10–30 KB/s with a 185-byte MTU and a 16-block
window. The screen shows the percentage while it runs (`Bluetooth 42%`), and the
queued note uploads over Wi-Fi on its own the moment the device sees the network
again, so a phone sync is for the notes you want *now*.

## 7. On the device

- The status line reads `Bluetooth <n>%` while a phone is taking a note, and adds
  ` · BT` to the counts line when a phone is connected and idle. An e-paper panel
  is repainted at most every 10 percentage points.
- `notes ble` prints the link state (`link up mtu 185`), the note in flight and
  why the last transfer stopped.
- `notes status` ends with `| ble idle|away|42%|off`.
- The device logs each command (`hg.notes: phone: get next`) and each delivery
  (`hg.notes: note delivered over Bluetooth`).

## 8. Setup

Bluetooth is on only for this board, and it does not fit in the shared 1.9 MB
app slots: NimBLE plus this service is about 219 KB (app 1,806,123 → 2,030,667
bytes, measured on a clean build). The board therefore builds with
`partitions-epaper-8mb.csv`, two 3.875 MB slots, and the app uses 1,984 KB of one
of them (50% free).

Changing the partition table needs **one USB flash**; an over-the-air update
cannot move partitions. `nvs` keeps its offset, so settings and paired phones
survive.

```sh
cd firmware/esp32
pio run -e waveshare-epaper-154                 # build
pio run -e waveshare-epaper-154 -t upload       # USB-C, once, for the new table
```

Then pair a phone once: **Settings → Phone app** (or `phone pair` over the
console) opens a two-minute window; the phone connects, the device asks whether
to allow it, and only after that does it show the passkey to confirm. The whole
first-time walk-through is [Voice notes off the gadget with an iPhone](ios-bluetooth-sync.md).

## 9. The phone side

### 9.1 Today: the relay page (works with no app of your own)

`web/ble-note-relay/` is a single-page Web Bluetooth client: it connects to the
service, lists the notes, takes one, checks every block and the SHA-256, posts it
to Hermes with the same multipart request and HMAC the device would have used,
and only then sends `done`.

| Where | Browser |
|---|---|
| iPhone | **Bluefy** (a free App Store browser that supports Web Bluetooth). Safari itself has no Web Bluetooth, on iOS or macOS. |
| Android | Chrome |
| Laptop | Chrome or Edge |

Setup:

1. Serve the page over **HTTPS**. Web Bluetooth only works in a secure origin,
   and on iOS that means `https://` (Bluefy has no exception): a plain
   `http://` LAN address is refused. The supported way is the container in
   [deploy/bluetooth-sync](../deploy/bluetooth-sync/README.md), which serves this
   page and `POST /notes/v1` from one HTTPS origin and fills the endpoint in for
   you. For a quick look on a laptop only,
   `cd web/ble-note-relay && python3 -m http.server 8000` works at
   `http://localhost:8000/`.
2. Fill in the notes endpoint, the **device id** and the **device key**. The key
   is the board's, from the host's device store:

   ```sh
   python3 - <<'PY'
   import json, pathlib
   store = pathlib.Path.home() / ".hermes/plugin-data/gadget/devices.json"
   for device_id, record in json.loads(store.read_text())["devices"].items():
       print(device_id, record["key"])
   PY
   ```

   The page keeps all four values in `localStorage` on the phone. The key can
   post notes as that device, so treat the phone like the device.
3. **Connect to the gadget** → pick the device → **List notes** → **Send** on a
   note (or **Fetch every note**). The page logs the HTTP status it got.

Because the relay signs as the board, the note's `idempotency_key` (its own id)
makes a double delivery harmless: a note already in Hermes answers `200`, and the
relay still sends `done`.

### 9.2 Pairing, and the acknowledgement on the device

The note service needs the same paired, encrypted link as the console channel, so
the first phone goes through the device's pairing window — and through an
acknowledgement that only someone holding the device can give:

1. **Settings → Phone app** on the device (or `phone pair`) opens the window for
   two minutes and starts advertising.
2. The phone connects and asks to pair. The device does **not** answer it: the
   screen shows *Allow this phone?* and the phone's Bluetooth address, and waits
   up to 20 s.
3. A tap on the screen allows it (the talk control on a board without touch, or
   `phone allow` on the console). A swipe down, `phone deny`, the window closing,
   or the 20 s running out refuses it: that link is dropped and the phone's
   pairing fails with an authentication error. A second phone asking while one
   waits is refused outright.
4. Allowed, the device shows a six-digit passkey; iOS shows the same code to
   confirm. That is LE Secure Connections with passkey entry — the passkey proves
   the link is not being relayed, and the acknowledgement proves that somebody at
   the device wanted *this* phone paired.

The order is the point: without the tap there is no code and no bond, so a phone
in radio range cannot pair itself with a device nobody is holding, and the address
on the screen is something to compare rather than decoration. Bonds persist (up
to three phones, oldest forgotten); `phone forget` removes them all. The state is
visible: `phone` (console or `status`) reports `false`/`true` plus the address
waiting (`confirm`) or the code (`code`).

**Why not a Shortcut alone.** An iOS Shortcut cannot open a Bluetooth LE
connection to a GATT service, and it has no HMAC-SHA256 action with which to sign
`POST /notes/v1`; a Shortcut therefore cannot be the receiver, and the board's key
must not be pasted into one that posts unsigned. The bridge is the Bluefy page in
§9.1: it holds the key on the phone (in `localStorage`, like the device holds it),
pulls the note over BLE, and signs the upload. A Shortcut can still be chained
after it, but it would only be doing what the page already did.

Any other BLE central follows the same contract: connect to `dc5300a0-…`,
subscribe to `dc5300a2-…`, write the commands from §2 to `dc5300a1-…`, and hand
the resulting WAV to the notes endpoint (§9.3).

### 9.3 What the phone posts

The same request the device makes, per the
[voice-note upload contract](voice-note-ingestion.md): `multipart/form-data` with
`audio` (the WAV), `device_id`, `recorded_at`, `duration_seconds`,
`idempotency_key` (the note id) and `checksum` (SHA-256 of the audio), signed with
`X-Gadget-Ts` and `X-Gadget-Mac`. `web/ble-note-relay/relay.mjs` implements it in
about 40 lines with WebCrypto, and `selftest.mjs` checks the signature against
Node's `crypto`.

## 10. Test steps

Device, with a card in and at least one note recorded:

1. `notes status` → `| ble away`.
2. **Phone app** → pairing open → pair from the phone using the passkey shown.
3. `notes status` → `| ble idle` when the phone is connected; the counts line
   gains ` · BT`.
4. `notes ble` → `link up mtu <n>`.
5. From the phone: **List notes** → the pending notes, with the byte counts and
   dates the console's `notes list` shows.
6. **Send** a note: the screen counts up (`Bluetooth 12%` … `100%`), the page logs
   `POST … -> 202`, and then `notes status` shows one fewer pending, one more
   sent, and `hg.notes: note delivered over Bluetooth` in the log.
7. Interrupt one: walk out of range mid-transfer. The note stays pending, the
   screen returns to the counts, and the phone logs the reason. Come back and
   press **Send** again: it resumes, and the note that lands in Hermes is the whole
   recording (the SHA-256 check passes).
8. Record a note with the phone out of range, then sync over Wi-Fi: unchanged
   behaviour.
9. Empty note: `notes record` then `notes stop` within a fraction of a second,
   and send it — `empty note <id>` on the phone, nothing transferred.
10. Unpaired phone: it cannot even write a command (the GATT write is refused).

Host, without hardware: `ctest -R hg_epaper_ble_tests` (the protocol) and
`node --test web/ble-note-relay/selftest.mjs` (the phone's half against the
firmware's own packets).

Host, without hardware, through the real endpoint (what the checkout can prove
today; the radio is still the hardware pass's):

```sh
# 1. Frame a real recording the way the device would, and post it exactly as the
#    relay page does — reassembled from the packets, signed with the board key.
cmake --build build/host --target hg_ble_capture
./build/host/hg_ble_capture tests/fixtures/voice-notes/sample.wav > /tmp/note.txt
# 2. Point relay.mjs's postNote at a notes endpoint (a scratch inbox and vault, or
#    the live one) and check the answer: 202 (or 200 for a repeat) and
#    shaMatchesHeader true.
```

Done on 2026-11-20 against `plugin/notes_ingest.py` running with a scratch inbox
and a scratch vault, with a real enrolled device key: 165,794 B in 970 blocks,
every block CRC checked, the header's SHA-256 equal to the rebuilt audio's,
`202 accepted`, the note transcribed and written as markdown into the vault, and
the job moved from `notes/inbox/` to `notes/done/`. What that does not cover is
the BLE link itself — pairing, the MTU, a dropped connection.

## 11. What has been verified, and what has not

Verified:

- The firmware builds clean for the board with Bluetooth on: 2,030,671 B (1,984 KB)
  in the 3,968 KB `ota_0` slot, 50% free, 86,492 B static RAM (26.4%), 105 s from
  an empty `.pio/build` (`pio run -e waveshare-epaper-154`).
- The partition table in the built image is the 8 MB one (ota_0 at `0x20000`,
  ota_1 at `0x400000`, 3,968 KB each), read back out of `partitions.bin`.
- The GATT table registers on real NimBLE, on the board: the boot log has
  `I hg.phone: Bluetooth ready` at 1282 ms and no `E (` line at all, `phone`
  answers `@phone {"paired":0,"connected":false,"pairing":false}`, `notes ble`
  answers `@notes ble link down mtu 23 idle`, `notes status` ends `| ble away`,
  and `phone pair` answers `@ok phone pairing open` with `GAP procedure
  initiated: advertise`. This one is new: `0588422` failed it — the data
  characteristic had no `access_cb`, NimBLE rejected the table with
  `ble_gatts_count_resources rc=3` and no service registered at all
  ([QA report](qa-report-0588422.md), D1).
- The protocol: 16 host cases (windows, resume, rewind, per-block CRC, empty
  notes, no card, unknown id, timeouts, MTU refusal, `done` moving the note).
- The GATT tables themselves: 3 host cases over `port_ble.cpp` — every
  characteristic that names a UUID names an access callback (NimBLE's own rule),
  the note data characteristic is notify-only behind `on_note_access`, and the
  test fails loudly if it stops recognising the tables. `port_ble.cpp` needs the
  ESP-IDF to compile, so this reads the source rather than linking it; it is what
  would have caught D1 before the board did.
- The phone's half, against a packet stream captured from the firmware itself:
  11 cases, including the SHA-256 of the rebuilt WAV. Re-running
  `hg_ble_capture` reproduces the committed fixture byte for byte, so the two
  halves are tested against the code in the tree, not a stale dump.
- The upload half end to end: a real 3.4 s WAV (165,794 B) framed by the firmware's
  own protocol into 970 blocks, reassembled and signed by `relay.mjs`, posted to
  the plugin's real `POST /notes/v1` — `202 accepted`, transcribed, markdown in the
  vault, job in `notes/done/` (steps in §10).
- The whole repository test set on this tree: `ctest` 8/8 (core, board, e-paper
  mode, BLE protocol, GATT table, notes, tailnet), `node --test
  web/ble-note-relay/selftest.mjs` 11/11, `npm test` in `site/` 32/32.

Not yet verified (needs a phone with `web/ble-note-relay/` open in Bluefy; no
iPhone and no BLE central was available to any pass so far):

- Pairing from an iPhone, the passkey entry, the MTU iOS negotiates, real
  throughput and resumption, a note actually pulled and posted from a phone, and
  the e-paper screen during a transfer. The board's half is proven to initialise
  and advertise; nothing past the radio handshake has been run against a phone.
- Pairing once more after the USB flash: the NVS partition keeps its offset, so
  paired phones *should* survive, but no phone has been paired, so bond survival
  has not been observed (settings in NVS did survive two USB flashes).
- Wi-Fi and Bluetooth together during a transfer (the board sets
  `CONFIG_ESP_COEX_SW_COEXIST_ENABLE`): both radios were up at once with no
  errors, and a note uploaded over Wi-Fi after the fix, but no note was sent over
  Bluetooth while Wi-Fi was transferring.

## The radio rests, and how to wake it

This board has no 32.768 kHz crystal, so with the Bluetooth controller enabled
the chip cannot light-sleep (`BLE_INIT: light sleep mode will not be able to
apply when bluetooth is enabled`). Bluetooth stays compiled into every build, but
the controller only runs inside a short window:

| Window | How long | Why |
|---|---|---|
| Boot | 2 minutes | a phone that is looking finds a new device |
| Pairing (`phone pair`, Settings → Phone app) | 2 minutes | the pairing exchange and its passkey |
| After a note is recorded | 3 minutes | a phone can take the note over BLE |
| Notes still waiting, no phone | 30 s every 5 minutes | keeps trying without holding the radio up |
| `phone wake` over the console | 1 minute | look for me now |

A connected phone keeps the radio up for as long as the link lasts. At rest the
controller is stopped and the board light-sleeps, which is why a phone must be
paired inside a window: if the app finds nothing, open the pairing window on the
device first (or run `phone wake` and retry).
