# Hermes note relay (Bluetooth → Hermes)

A single page that takes a voice note off the e-paper gadget over Bluetooth LE
and posts it to Hermes, for when the device has no Wi-Fi at all. The protocol and
the device-side rules are in [docs/ble-note-sync.md](../../docs/ble-note-sync.md).

| File | What it is |
|---|---|
| `index.html` | the page: settings, connect, the note list, the log |
| `relay.mjs` | the protocol (packets, CRC, header, multipart, HMAC) with no Bluetooth in it |
| `selftest.mjs` | 11 Node cases, run against `test/transfer-fixture.txt` |
| `test/transfer-fixture.txt` | the device's own packet stream, dumped by `firmware/tools/ble_sync_capture.cpp` |

## Run it

The supported way is the container in
[deploy/bluetooth-sync](../../deploy/bluetooth-sync/README.md): it serves this
folder over HTTPS from the same origin as `POST /notes/v1`, which is what Web
Bluetooth requires, and pre-fills the endpoint. Open
`https://<server-name>:8443/` in **Bluefy** on the iPhone (Safari has no Web
Bluetooth); on a computer, Chrome or Edge.

To look at the page without a server (a laptop, `localhost` only):

```sh
cd web/ble-note-relay
python3 -m http.server 8000       # then open http://localhost:8000/
```

Fill in:

- **Notes endpoint** — `http://<hub-host>:8766/notes/v1`.
- **Device id** — the board's, as `hermes gadget devices` shows it.
- **Device key** — the board's key from the host:

  ```sh
  python3 - <<'PY'
  import json, pathlib
  store = pathlib.Path.home() / ".hermes/plugin-data/gadget/devices.json"
  for device_id, record in json.loads(store.read_text())["devices"].items():
      print(device_id, record["key"])
  PY
  ```

  It lives in that page's `localStorage` afterwards. It can post notes as the
  board, so treat the phone like the board: clear the field if you hand it over.

- **Access token** — only if the host sets `GADGET_ACCESS_TOKEN`.

## What it does with a note

1. `list` — the notes waiting in `pending/`, oldest first.
2. `get <id>` (or `get next`) — the header, then blocks of about 171 bytes.
3. Checks every block's CRC-32 and writes it at the offset it carries;
   acknowledges what it has as it goes.
4. On the end packet: checks the WAV against the SHA-256 in the header, then posts
   the six-part multipart request with `X-Gadget-Ts` and `X-Gadget-Mac`.
5. Only when Hermes answered 2xx: `done <id>`, which moves the note to `sent/` on
   the device.

If the link drops, the bytes already stored are kept and **Send** again resumes
from there (`resume <id> <offset>`). Nothing is deleted from the device until
Hermes has the note.

## Tests

```sh
node --test selftest.mjs
```

The fixture is the firmware's own output, not a copy of the format — regenerate it
after changing `notes_ble.cpp`:

```sh
cmake --build <host build dir> --target hg_ble_capture
./<host build dir>/hg_ble_capture > web/ble-note-relay/test/transfer-fixture.txt
```
## The phone lists no device

The page asks the browser for devices advertising the note service
(`dc5300a0-…`), and Chrome and iOS only offer devices whose advertising packet
carries it. Two things make the list empty:

- the gadget's **Phone app** pairing window is not open. The device advertises
  only while that window is open, or once a phone is already paired;
- the firmware advertises the wrong service. It must be `…00a0`, not the hub's
  `…0001`.

`tools/scan_adv.py` answers that from any machine with a Bluetooth adapter:

```sh
pip install bleak
python3 tools/scan_adv.py        # with the pairing window open
```

It prints the name, RSSI and service UUIDs of every device advertising `…00a0`.
An empty result means the packet does not carry it (or nothing is advertising),
which no phone setting can fix.
